#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "Console/ConsoleCommand.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "Resources/RenderTargetLayouts.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Scene.h"
#include "SceneViewProjection.h"
#include "Terrain/TerrainHeightmap.h"
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

#include <algorithm>

namespace Durin
{
	namespace
	{
		std::vector<FGPUTimingQueryRHIRef>* GPostProcessTimingQueries = nullptr;
		FViewRenderTelemetry GLastViewTelemetry;
		size_t GViewTelemetryCaptureCount = 0;

		auto CaptureViewTelemetry(const FViewRenderTelemetry& Telemetry) -> void
		{
			GLastViewTelemetry = Telemetry;
			++GViewTelemetryCaptureCount;
		}

		auto CapturePostProcessTiming(
			const FGPUTimingQueryRHIRef& Query
		) -> void
		{
			if (GPostProcessTimingQueries != nullptr)
				GPostProcessTimingQueries->push_back(Query);
		}

		struct FRenderEditorGridCapture
		{
			static constexpr auto GetName() -> const char*
			{
				return "RenderEditorGridCapture";
			}
		};

		struct FCaptureHDRDisplayContract
		{
			static constexpr auto GetName() -> const char*
			{
				return "CaptureHDRDisplayContract";
			}
		};

		struct FFailDisplayPayloadContract
		{
			static constexpr auto GetName() -> const char*
			{
				return "FailDisplayPayloadContract";
			}
		};

		struct FContactVisibilityTargetLifecycle
		{
			static constexpr auto GetName() -> const char*
			{
				return "ContactVisibilityTargetLifecycle";
			}
		};

		auto BuildViewMatrix(
			const FVector3& Location,
			const FVector3& Forward
		) -> FMatrix
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

		auto MakeGridView(const FVector3& Forward) -> FSceneView
		{
			FSceneView View;
			View.ViewLocation = {-5.0, -5.0, 3.0};
			View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Math::Normalize(Forward));
			EXPECT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
				60.0, 1.0, 0.1, 500000.0,
				ESceneDepthConvention::ReversedZ,
				View.ProjectionMatrix
			));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = 129;
			View.ViewportHeight = 129;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.0f, 0.0f, 0.0f, 1.0f};
			View.Settings.Mode.RenderMode = ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode =
				EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.Mode.LODMode = EViewLODMode::ForceLOD0;
			View.EditorGrid.bVisible = true;
			View.EditorGrid.Height = 0.0;
			View.EditorGrid.FadeDistance = 1000.0f;
			View.EditorGrid.MinorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.MajorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.AxisXColor = {1.0f, 0.0f, 0.0f, 1.0f};
			View.EditorGrid.AxisYColor = {0.0f, 1.0f, 0.0f, 1.0f};
			return View;
		}

		auto CountVisiblePixels(const std::vector<std::byte>& Pixels) -> size_t
		{
			size_t Result = 0;
			for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
			{
				Result += Pixels[Offset] > std::byte{16}
								  || Pixels[Offset + 1] > std::byte{16}
								  || Pixels[Offset + 2] > std::byte{16} ?
							  1u :
							  0u;
			}
			return Result;
		}

		auto MakeGBufferVertexDeclaration(
			EGBufferVertexDomain Domain
		) -> FVertexDeclarationRHIRef
		{
			FVertexDeclarationElementList Elements{};
			if (Domain == EGBufferVertexDomain::Terrain)
			{
				Elements[0] = FVertexElement(
					0, 0, EVertexElementType::UShort2, 0, 4
				);
				return GDynamicRHI->RHICreateVertexDeclaration(Elements);
			}
			Elements[0] = FVertexElement(
				0, 0, EVertexElementType::Float3, 0, 12
			);
			Elements[1] = FVertexElement(
				1, 0, EVertexElementType::Short4N, 1, 16
			);
			Elements[2] = FVertexElement(
				1, 8, EVertexElementType::Short4N, 2, 16
			);
			for (uint8 Channel = 0; Channel < 4; ++Channel)
			{
				Elements[3 + Channel] = FVertexElement(
					2, static_cast<uint16>(Channel * 8),
					EVertexElementType::Float2,
					static_cast<uint8>(3 + Channel), 32
				);
			}
			Elements[7] = FVertexElement(
				3, 0, EVertexElementType::UByte4N, 7, 4
			);
			if (Domain == EGBufferVertexDomain::Skeletal)
			{
				Elements[8] = FVertexElement(
					4, 0, EVertexElementType::UShort4, 8, 24
				);
				Elements[9] = FVertexElement(
					4, 8, EVertexElementType::Float4, 9, 24
				);
			}
			return GDynamicRHI->RHICreateVertexDeclaration(Elements);
		}

		auto RenderGridCapture(
			FRendererModule& Renderer,
			FScene* Scene,
			const FVector3& Forward,
			bool bLit = false
		) -> std::vector<std::byte>
		{
			auto Pixels = std::make_shared<std::vector<std::byte>>();
			EnqueueRenderCommand<FRenderEditorGridCapture>(
				[&Renderer, Scene, Forward, Pixels, bLit](
					FRHICommandListImmediate& CommandList
				) {
					GRenderFrameCounterRenderThread++;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
										  "EditorGridValidationColor", 129, 129,
										  EPixelFormat::SRGBA8_UNORM
					)
										  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Target =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);

					FSceneView View = MakeGridView(Forward);
					if (bLit) View.Settings.Mode.RenderMode = ERenderMode::Lit;
					FSceneViewRenderOptions Options;
					EXPECT_EQ(Renderer.RenderView(CommandList, Scene, View, Target, false, Options), ERenderViewResult::Success);
					ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Target, 0, 0, *Pixels
					));
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			FlushRenderingCommands();
			return std::move(*Pixels);
		}
	} // namespace

	TEST(FEditorGridVulkanTests, ReversedZGridRemainsStableAcrossCoplanarRotatedViews)
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
		InitRenderingThread();
		FRendererModule Renderer;
		Durin::FModuleTestHarness RendererLifecycle("EditorGridRendererTest");
		RendererLifecycle.Start(Renderer);

		auto FailureResults = std::make_shared<
			std::array<ERenderViewResult, 2>>();
		auto FailurePixelsBefore = std::make_shared<std::vector<std::byte>>();
		auto FailurePixelsAfter = std::make_shared<std::vector<std::byte>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::GraphicsPipeline
		);
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Renderer, FailureResults, FailurePixelsBefore, FailurePixelsAfter](
				FRHICommandListImmediate& CommandList
			) {
				GRenderFrameCounterRenderThread++;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = FRHITextureCreateDesc::Create2D(
									  "FailedDisplayPayloadOutput", 1, 1,
									  EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
				FTextureRHIRef Output =
					GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Output, nullptr);
				FRHIRenderPassInfo SentinelPass{};
				SentinelPass.RenderTargetLayout =
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen
					);
				SentinelPass.ColorRenderTargets[0] = Output;
				SentinelPass.ColorClearValues[0] =
					FClearValueBinding(0.25f, 0.5f, 0.75f, 1.0f);
				CommandList.BeginRenderPass(
					SentinelPass, "FailedDisplayPayloadSentinelPass"
				);
				CommandList.EndRenderPass();
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, Output, 0, 0, *FailurePixelsBefore
				));
				FSceneView View;
				View.ClearColor = {4.0f, 2.0f, 1.0f, 1.0f};
				View.Settings.PostProcess.bEnableFXAA = false;
				(*FailureResults)[0] = Renderer.RenderView(
					CommandList, nullptr, View, Output, false, {}
				);
				(*FailureResults)[1] = Renderer.RenderView(
					CommandList, nullptr, View, Output, false, {}
				);
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, Output, 0, 0, *FailurePixelsAfter
				));
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		FlushRenderingCommands();
		EXPECT_EQ((*FailureResults)[0], ERenderViewResult::RendererResourcesUnavailable);
		EXPECT_EQ((*FailureResults)[1], ERenderViewResult::RendererResourcesUnavailable);
		EXPECT_EQ(*FailurePixelsAfter, *FailurePixelsBefore);
		EXPECT_TRUE(FConsoleCommandRegistry::Get().Execute(
													  "renderer.retry-resources"
		)
						.bSuccess);
		FlushRenderingCommands();

		auto HDRPixels = std::make_shared<std::vector<std::byte>>();
		auto DefaultExposurePixels = std::make_shared<std::vector<std::byte>>();
		auto LowExposurePixels = std::make_shared<std::vector<std::byte>>();
		std::vector<FGPUTimingQueryRHIRef> PostProcessTimingQueries;
		GPostProcessTimingQueries = &PostProcessTimingQueries;
		SetPostProcessTimingQuerySink(CapturePostProcessTiming);
		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&Renderer, HDRPixels, DefaultExposurePixels, LowExposurePixels](
				FRHICommandListImmediate& CommandList
			) {
				GRenderFrameCounterRenderThread++;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto HDRDesc = FRHITextureCreateDesc::Create2D(
										 "HDRPreservationValidation", 1, 1,
										 EPixelFormat::RGBA16_FLOAT
				)
										 .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
				FTextureRHIRef HDRTarget =
					GDynamicRHI->RHICreateTexture(CommandList, HDRDesc);
				ASSERT_NE(HDRTarget, nullptr);
				FRHIRenderPassInfo HDRPass{};
				HDRPass.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferDebugOutput();
				HDRPass.ColorRenderTargets[0] = HDRTarget;
				HDRPass.ColorClearValues[0] =
					FClearValueBinding(4.0f, 2.0f, 0.5f, 0.5f);
				CommandList.BeginRenderPass(HDRPass, "HDRPreservationValidationPass");
				CommandList.EndRenderPass();
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, HDRTarget, 0, 0, *HDRPixels
				));

				auto CaptureDisplay = [&](const char* Name, float ExposureEV,
										  std::vector<std::byte>& Pixels) {
					const auto OutputDesc = FRHITextureCreateDesc::Create2D(
												Name, 1, 1, EPixelFormat::SRGBA8_UNORM
					)
												.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Output =
						GDynamicRHI->RHICreateTexture(CommandList, OutputDesc);
					EXPECT_NE(Output, nullptr);
					FSceneView View;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.PostProcess.bEnableFXAA = false;
					View.Settings.PostProcess.ExposureEV = ExposureEV;
					EXPECT_EQ(Renderer.RenderView(CommandList, nullptr, View, Output, false, {}), ERenderViewResult::Success);
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, Pixels
					));
				};
				CaptureDisplay(
					"DefaultExposureValidation", 0.0f,
					*DefaultExposurePixels
				);
				CaptureDisplay(
					"LowExposureValidation", -2.0f,
					*LowExposurePixels
				);
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		FlushRenderingCommands();
		SetPostProcessTimingQuerySink(nullptr);
		GPostProcessTimingQueries = nullptr;
		for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
		{
			const bool bReady = PostProcessTimingQueries.size() == 2
								&& std::ranges::all_of(
									PostProcessTimingQueries,
									[](const FGPUTimingQueryRHIRef& Query) {
										return Query->GetResult().State
											   == ERHIGPUTimingResultState::Ready;
									}
								);
			if (bReady) break;
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[](FRHICommandListImmediate& CommandList) {
					GRenderFrameCounterRenderThread++;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			FlushRenderingCommands();
		}
		ASSERT_EQ(HDRPixels->size(), 8u);
		auto ReadHalfBits = [&HDRPixels](size_t Channel) {
			const size_t Offset = Channel * 2;
			return std::to_integer<uint16>((*HDRPixels)[Offset])
				   | (std::to_integer<uint16>((*HDRPixels)[Offset + 1]) << 8);
		};
		EXPECT_EQ(ReadHalfBits(0), 0x4400u); // 4.0
		EXPECT_EQ(ReadHalfBits(1), 0x4000u); // 2.0
		EXPECT_EQ(ReadHalfBits(2), 0x3800u); // 0.5
		EXPECT_EQ(ReadHalfBits(3), 0x3800u); // alpha 0.5
		ASSERT_EQ(DefaultExposurePixels->size(), 4u);
		ASSERT_EQ(LowExposurePixels->size(), 4u);
		EXPECT_GT(std::to_integer<uint8>((*DefaultExposurePixels)[0]),
			std::to_integer<uint8>((*LowExposurePixels)[0]) + 10u);
		EXPECT_GT(std::to_integer<uint8>((*DefaultExposurePixels)[0]), 248u);
		EXPECT_GT((*DefaultExposurePixels)[1], (*LowExposurePixels)[1]);
		EXPECT_NEAR(std::to_integer<uint8>((*DefaultExposurePixels)[3]), 128u, 1u);
		EXPECT_NEAR(std::to_integer<uint8>((*LowExposurePixels)[3]), 128u, 1u);
		ASSERT_EQ(PostProcessTimingQueries.size(), 2u);
		for (const FGPUTimingQueryRHIRef& Query : PostProcessTimingQueries)
		{
			const FRHIGPUTimingResult Result = Query->GetResult();
			EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
		}
		PostProcessTimingQueries.clear();
		struct FViewRouteCase
		{
			const char* Name = nullptr;
			uint32 Width = 0;
			uint32 Height = 0;
			float ExposureEV = 0.0f;
			bool bEnableFXAA = false;
		};
		const std::array ViewRouteCases{
			FViewRouteCase{"MainView", 128, 72, 0.0f, false},
			FViewRouteCase{"AuxiliaryView", 64, 64, -2.0f, true},
			FViewRouteCase{"CameraPreview", 80, 45, 1.0f, false},
			FViewRouteCase{"AssetThumbnail", 96, 96, 0.0f, true}
		};
		auto CaptureViewRoute = [&Renderer](const FViewRouteCase& Route, bool bGBufferDebug = false) {
			auto Pixels = std::make_shared<std::vector<std::byte>>();
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[&Renderer, Route, Pixels, bGBufferDebug](
					FRHICommandListImmediate& CommandList
				) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
										  Route.Name,
										  Route.Width,
										  Route.Height,
										  EPixelFormat::SRGBA8_UNORM
					)
										  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Output =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Output, nullptr);
					FSceneView View;
					View.ViewportWidth = Route.Width;
					View.ViewportHeight = Route.Height;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.PostProcess.ExposureEV = Route.ExposureEV;
					View.Settings.PostProcess.bEnableFXAA = Route.bEnableFXAA;
					FSceneViewRenderOptions Options;
					Options.GBufferDebugMode = bGBufferDebug ? EGBufferDebugMode::Flags : EGBufferDebugMode::Disabled;
					FScopedRendererQualificationPolicy Qualification({
						.bEnableDeferredDirectional = bGBufferDebug});
					EXPECT_EQ(Renderer.RenderView(CommandList, nullptr, View, Output, false, Options), ERenderViewResult::Success);
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, *Pixels
					));
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			FlushRenderingCommands();
			return std::move(*Pixels);
		};
		SetViewRenderTelemetrySink(CaptureViewTelemetry);
		std::array<std::vector<std::byte>, 4> RoutePixels;
		for (size_t Index = 0; Index < ViewRouteCases.size(); ++Index)
		{
			RoutePixels[Index] = CaptureViewRoute(ViewRouteCases[Index]);
			EXPECT_EQ(RoutePixels[Index].size(), static_cast<size_t>(ViewRouteCases[Index].Width) * ViewRouteCases[Index].Height * 4u);
			ASSERT_GE(RoutePixels[Index].size(), 4u);
			EXPECT_NEAR(std::to_integer<uint8>(RoutePixels[Index][3]), 128u, 1u);
			EXPECT_EQ(GLastViewTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
			EXPECT_EQ(GLastViewTelemetry.Deferred.HybridDeferredUnavailableViews, 0u);
		}
		EXPECT_LT(RoutePixels[1][0], RoutePixels[0][0]);
		EXPECT_GT(RoutePixels[2][2], RoutePixels[0][2]);
		EXPECT_EQ(RoutePixels[3][0], RoutePixels[0][0]);
		EXPECT_EQ(RoutePixels[3][1], RoutePixels[0][1]);
		EXPECT_EQ(RoutePixels[3][2], RoutePixels[0][2]);
		const std::vector<std::byte> MainAfterOtherViews =
			CaptureViewRoute(ViewRouteCases.front());
		EXPECT_EQ(MainAfterOtherViews, RoutePixels.front());
		const std::array<size_t, 5> GBufferRouteOrder{3u, 1u, 2u, 0u, 3u};
		std::vector<std::byte> FirstThumbnailDebug;
		for (size_t OrderIndex = 0;
			 OrderIndex < GBufferRouteOrder.size(); ++OrderIndex)
		{
			const FViewRouteCase& Route =
				ViewRouteCases[GBufferRouteOrder[OrderIndex]];
			const std::vector<std::byte> DebugPixels =
				CaptureViewRoute(Route, true);
			EXPECT_EQ(DebugPixels.size(), static_cast<size_t>(Route.Width) * Route.Height * 4u);
			EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferEnabledViews, 1u);
			EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferDebugViews, 1u);
			EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferDebugFailures, 0u);
			EXPECT_EQ(
				GLastViewTelemetry.Deferred.DeferredDirectionalEnabledViews, 1u
			);
			EXPECT_EQ(
				GLastViewTelemetry.Deferred.DeferredDirectionalUnavailableViews, 0u
			);
			EXPECT_EQ(
				GLastViewTelemetry.Deferred.DeferredDirectionalPassFailures, 0u
			);
			EXPECT_EQ(GLastViewTelemetry.Deferred.DeferredDirectionalOutputBytes, FDeferredDirectionalLightingRenderer::CalculateTargetBytes(Route.Width, Route.Height));
			EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferAttachmentBytes, FGBufferRenderer::CalculateTargetBytes(Route.Width, Route.Height));
			if (OrderIndex == 0u)
				FirstThumbnailDebug = DebugPixels;
			if (OrderIndex + 1u == GBufferRouteOrder.size())
				EXPECT_EQ(DebugPixels, FirstThumbnailDebug);
		}
		SetViewRenderTelemetrySink(nullptr);

		const std::array<uint16, 9> Samples{};
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		std::string Error;
		ASSERT_TRUE(BuildTerrainHeightmapPayload(
			3, 3, Samples, Payload, Error
		)) << Error;
		auto* MaterialObject = NewObject<DMaterial>(
			nullptr, "EditorGridTerrainMaterial");
		ASSERT_TRUE(MaterialObject->SetStaticProperties(FMaterialStaticProperties{
			.BlendMode = EMaterialBlendMode::Opaque,
			.ShadingModel = EMaterialShadingModel::Unlit,
			.bTwoSided = true
		}));
		ASSERT_TRUE(MaterialObject->SetVectorParameterValue(
			MaterialParameters::BaseColorName(), {0.0f, 0.0f, 0.0f}));
		auto Material = MaterialObject->GetMaterialRenderProxy();
		FlushRenderingCommands();
		constexpr double TerrainExtent = 4000.0;
		FTerrainPatchDescriptor Patch{
			.OriginX = 0,
			.OriginY = 0,
			.CellCountX = 2,
			.CellCountY = 2,
			.LODSteps = {1},
			.LODErrors = {0.0},
			.LocalBounds = FBox(
				{0.0, 0.0, 0.0}, {TerrainExtent, TerrainExtent, 0.0}
			)
		};
		FScene Scene;
		Scene.AddOrReplacePrimitive(
			FPrimitiveSceneId(1),
			std::make_unique<FTerrainSceneProxy>(Payload, 1, TerrainExtent * 0.5, TerrainExtent * 0.5, 1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch}, Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.0
			})
		);
		FScene OccluderScene;
		OccluderScene.AddOrReplacePrimitive(
			FPrimitiveSceneId(2),
			std::make_unique<FTerrainSceneProxy>(Payload, 1, TerrainExtent * 0.5, TerrainExtent * 0.5, 1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch}, Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.25
			})
		);
		FlushRenderingCommands();

		const std::array<FVector3, 5> CameraDirections = {{
			{1.0, 1.0, -0.5},
			{1.0, 0.85, -0.5},
			{0.85, 1.0, -0.5},
			{1.0, 1.0, -0.42},
			{1.0, 1.0, -0.58},
		}};
		for (const FVector3& Forward : CameraDirections)
		{
			const std::vector<std::byte> EmptyPixels =
				RenderGridCapture(Renderer, nullptr, Forward);
			const std::vector<std::byte> TerrainPixels =
				RenderGridCapture(Renderer, &Scene, Forward);
			ASSERT_EQ(EmptyPixels.size(), 129u * 129u * 4u);
			ASSERT_EQ(TerrainPixels.size(), EmptyPixels.size());
			const size_t EmptyVisible = CountVisiblePixels(EmptyPixels);
			const size_t TerrainVisible = CountVisiblePixels(TerrainPixels);
			ASSERT_GT(EmptyVisible, 0u);
			EXPECT_GE(TerrainVisible, EmptyVisible * 99u / 100u);
		}
		const std::vector<std::byte> OccludedPixels = RenderGridCapture(
			Renderer, &OccluderScene, CameraDirections.front()
		);
		EXPECT_EQ(CountVisiblePixels(OccludedPixels), 0u);
		SetViewRenderTelemetrySink(CaptureViewTelemetry);
		const std::vector<std::byte> TerrainHybridLit = RenderGridCapture(
			Renderer, &Scene, CameraDirections.front(), true
		);
		SetViewRenderTelemetrySink(nullptr);
		EXPECT_EQ(TerrainHybridLit.size(), 129u * 129u * 4u);
		EXPECT_EQ(GLastViewTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
		EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferTerrainSkippedDraws, 1u);

		RendererLifecycle.Shutdown();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, GBufferTargetsRecoverAtomicallyAfterInjectedImageFailure)
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
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FRendererTransientTargetPool TransientTargets(Coordinator);
		FGBufferRenderer GBuffer(Coordinator, TransientTargets);
		auto* MaterialObject = NewObject<DMaterial>(
			nullptr, "GBufferRecoveryMaterial");
		const FMaterialRenderData MaterialData = MaterialObject->GetRenderData();
		ASSERT_TRUE(MaterialData.CompiledProgram);
		auto bFailed = std::make_shared<bool>(false);
		auto bSuppressed = std::make_shared<bool>(false);
		auto RecoveredTargets =
			std::make_shared<FGBufferRenderer::FTargets>();
		auto AlternateTargets =
			std::make_shared<FGBufferRenderer::FTargets>();
		auto PipelineResults =
			std::make_shared<std::array<bool, 13>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::Image
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::ShaderModule
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::GraphicsPipeline
		);
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Coordinator, &TransientTargets, &GBuffer, bFailed, bSuppressed,
			 RecoveredTargets, AlternateTargets, PipelineResults, MaterialData](
				FRHICommandListImmediate&
			) {
				*bFailed = !GBuffer.EnsureTargets_RenderThread(64, 32);
				*bSuppressed =
					!GBuffer.EnsureTargets_RenderThread(64, 32);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto Recovered =
					GBuffer.EnsureTargets_RenderThread(64, 32);
				ASSERT_TRUE(Recovered);
				*RecoveredTargets = *Recovered;
				auto Alternate =
					GBuffer.EnsureTargets_RenderThread(32, 16);
				ASSERT_TRUE(Alternate);
				*AlternateTargets = *Alternate;

				FRHIDepthStencilState Depth;
				Depth.bEnableTest = true;
				Depth.bEnableWrite = true;
				Depth.CompareOp = ERHIDepthCompareOp::Greater;
				const std::array<EGBufferVertexDomain, 4> Domains{
					EGBufferVertexDomain::Local,
					EGBufferVertexDomain::Spline,
					EGBufferVertexDomain::Skeletal,
					EGBufferVertexDomain::Terrain
				};
				std::array<FVertexDeclarationRHIRef, 4> Declarations;
				for (size_t Index = 0; Index < Domains.size(); ++Index)
				{
					Declarations[Index] =
						MakeGBufferVertexDeclaration(Domains[Index]);
					ASSERT_NE(Declarations[Index], nullptr);
				}
				auto MakeRequest = [&](size_t Index) {
					return FGBufferRenderer::FPipelineRequest{
						.Material = MaterialData.PlanningPassIdentity,
						.CompiledProgram = MaterialData.CompiledProgram,
						.Rasterizer = FRHIRasterizerState{},
						.Depth = Depth,
						.VertexDeclaration = Declarations[Index],
						.VertexDomain = Domains[Index]
					};
				};
				const auto LocalRequest = MakeRequest(0);
				(*PipelineResults)[0] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) == nullptr;
				(*PipelineResults)[1] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) == nullptr;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*PipelineResults)[2] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) == nullptr;
				(*PipelineResults)[3] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) == nullptr;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*PipelineResults)[4] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) != nullptr;
				for (size_t Index = 1; Index < Domains.size(); ++Index)
				{
					(*PipelineResults)[4 + Index] =
						GBuffer.EnsurePipeline_RenderThread(
							MakeRequest(Index)
						)
						!= nullptr;
				}
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ShaderChanged,
					FRendererResourceInvalidationTargets{}
				);
				(*PipelineResults)[8] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) != nullptr;
				FRHITexture* BeforeDevice =
					RecoveredTargets->Material.GetReference();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{}
				);
				auto DeviceTargets =
					GBuffer.EnsureTargets_RenderThread(64, 32);
				(*PipelineResults)[9] = DeviceTargets.has_value()
										&& DeviceTargets->Material.GetReference() != BeforeDevice;
				(*PipelineResults)[10] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) != nullptr;
				FRHITexture* BeforeRelease = DeviceTargets.has_value() ? DeviceTargets->Material.GetReference() : nullptr;
				GBuffer.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				auto ReleasedTargets =
					GBuffer.EnsureTargets_RenderThread(64, 32);
				(*PipelineResults)[11] = ReleasedTargets.has_value()
										 && ReleasedTargets->Material.GetReference() != BeforeRelease;
				(*PipelineResults)[12] =
					GBuffer.EnsurePipeline_RenderThread(LocalRequest) != nullptr;
			}
		);
		FlushRenderingCommands();

		EXPECT_TRUE(*bFailed);
		EXPECT_TRUE(*bSuppressed);
		const FGBufferRenderer::FTargets& Targets = *RecoveredTargets;
		ASSERT_NE(Targets.Material, nullptr);
		ASSERT_NE(Targets.Normals, nullptr);
		ASSERT_NE(Targets.Surface, nullptr);
		ASSERT_NE(Targets.Emissive, nullptr);
		EXPECT_EQ(Targets.Material->GetFormat(), EPixelFormat::RGBA8_UNORM);
		EXPECT_EQ(Targets.Normals->GetFormat(), EPixelFormat::RGBA8_UNORM);
		EXPECT_EQ(Targets.Surface->GetFormat(), EPixelFormat::RGBA8_UNORM);
		EXPECT_EQ(Targets.Emissive->GetFormat(), EPixelFormat::R11G11B10_FLOAT);
		EXPECT_EQ(Targets.Material->GetSizeX(), 64u);
		EXPECT_EQ(Targets.Material->GetSizeY(), 32u);
		ASSERT_NE(AlternateTargets->Material, nullptr);
		EXPECT_NE(AlternateTargets->Material, Targets.Material);
		EXPECT_NE(AlternateTargets->Normals, Targets.Normals);
		EXPECT_EQ(AlternateTargets->Material->GetSizeX(), 32u);
		EXPECT_EQ(AlternateTargets->Material->GetSizeY(), 16u);
		for (size_t Index = 0; Index < PipelineResults->size(); ++Index)
			EXPECT_TRUE((*PipelineResults)[Index]) << Index;

		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&TransientTargets, &GBuffer, RecoveredTargets, AlternateTargets](
				FRHICommandListImmediate&
			) {
				GBuffer.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				*RecoveredTargets = {};
				*AlternateTargets = {};
			}
		);
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, GroundTruthAmbientOcclusionTargetsRecoverAfterFailureAndInvalidation)
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
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FRendererTransientTargetPool TransientTargets(Coordinator);
		FFullscreenGeometryResources FullscreenGeometry;
		FGroundTruthAmbientOcclusionRenderer AmbientOcclusion(
			Coordinator, FullscreenGeometry, TransientTargets
		);
		auto Results = std::make_shared<std::array<bool, 14>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::Image
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::ShaderModule
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::GraphicsPipeline
		);
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Coordinator, &TransientTargets, &AmbientOcclusion,
				&FullscreenGeometry, Results](
				FRHICommandListImmediate& CommandList
			) {
				(*Results)[0] =
					!AmbientOcclusion.EnsureTargets_RenderThread(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[1] =
					!AmbientOcclusion.EnsureTargets_RenderThread(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto Recovered =
					AmbientOcclusion.EnsureTargets_RenderThread(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[2] = Recovered.has_value() && Recovered->Raw != nullptr
								&& Recovered->Scratch != nullptr
								&& Recovered->Raw->GetFormat() == EPixelFormat::R8_UNORM;
				FRHITexture* RecoveredRaw =
					Recovered.has_value() ? Recovered->Raw.GetReference() : nullptr;
				auto Alternate =
					AmbientOcclusion.EnsureTargets_RenderThread(32, 16,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[3] = Alternate.has_value() && Alternate->Raw != nullptr
								&& Alternate->Raw->GetSizeX() == 32
								&& Alternate->Raw->GetSizeY() == 16;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{}
				);
				auto DeviceTargets =
					AmbientOcclusion.EnsureTargets_RenderThread(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[4] = DeviceTargets.has_value()
								&& DeviceTargets->Raw.GetReference() != RecoveredRaw;
				FRHITexture* DeviceRaw = DeviceTargets.has_value() ? DeviceTargets->Raw.GetReference() : nullptr;
				AmbientOcclusion.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				auto ReleasedTargets =
					AmbientOcclusion.EnsureTargets_RenderThread(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[5] = ReleasedTargets.has_value()
								&& ReleasedTargets->Raw.GetReference() != DeviceRaw;
				(*Results)[6] = ReleasedTargets.has_value()
								&& ReleasedTargets->Raw->GetSizeX() == 64
								&& ReleasedTargets->Raw->GetSizeY() == 32;
				(*Results)[7] =
					FGroundTruthAmbientOcclusionRenderer::CalculateRawTargetBytes(
						64, 32
					)
					== 2048;
				const auto ColorDesc = FRHITextureCreateDesc::Create2D(
										   "GroundTruthAmbientOcclusionFailureColor", 64, 32,
										   EPixelFormat::RGBA8_UNORM
				)
										   .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
				const auto DepthDesc = FRHITextureCreateDesc::Create2D(
										   "GroundTruthAmbientOcclusionFailureDepth", 64, 32,
										   EPixelFormat::D32
				)
										   .SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::ShaderResource);
				FTextureRHIRef Normals = RHICreateTexture(ColorDesc);
				FTextureRHIRef Surface = RHICreateTexture(ColorDesc);
				FTextureRHIRef Depth = RHICreateTexture(DepthDesc);
				ASSERT_NE(Normals, nullptr);
				ASSERT_NE(Surface, nullptr);
				ASSERT_NE(Depth, nullptr);
				const FRHITextureSubresourceRange ColorRange{
					ERHITextureAspect::Color, 0, 1, 0, 1
				};
				const FRHITextureSubresourceRange DepthRange{
					ERHITextureAspect::Depth, 0, 1, 0, 1
				};
				CommandList.TransitionTextures(std::array{
					FRHITextureTransition{Normals, ColorRange, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead},
					FRHITextureTransition{Surface, ColorRange, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead},
					FRHITextureTransition{Depth, DepthRange, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead}
				});
				FSceneView View;
				View.ViewportWidth = 64;
				View.ViewportHeight = 32;
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				auto RenderRaw = [&]() {
					return AmbientOcclusion.RenderRaw_RenderThread(
						CommandList, *ReleasedTargets, Normals, Surface, Depth, View
					);
				};
				(*Results)[8] = !RenderRaw();
				(*Results)[9] = !RenderRaw();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*Results)[10] = !RenderRaw();
				(*Results)[11] = !RenderRaw();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*Results)[12] = RenderRaw();
				auto HalfTargets = AmbientOcclusion.EnsureTargets_RenderThread(
					65, 33,
					EGroundTruthAmbientOcclusionQuality::HalfResolution);
				(*Results)[13] = HalfTargets.has_value()
					&& HalfTargets->Raw->GetSizeX() == 33
					&& HalfTargets->Raw->GetSizeY() == 17
					&& HalfTargets->Selector != nullptr
					&& HalfTargets->Resolved->GetSizeX() == 65
					&& HalfTargets->Resolved->GetSizeY() == 33;
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				AmbientOcclusion.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				FullscreenGeometry.ReleaseResources_RenderThread();
			}
		);
		FlushRenderingCommands();
		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, ContactVisibilityTargetsRecoverAfterFailureAndInvalidation)
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
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FRendererTransientTargetPool TransientTargets(Coordinator);
		FFullscreenGeometryResources FullscreenGeometry;
		FContactShadowVisibilityRenderer ContactVisibility(
			Coordinator, FullscreenGeometry, TransientTargets);
		auto Results = std::make_shared<std::array<bool, 18>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::Image);
		EnqueueRenderCommand<FContactVisibilityTargetLifecycle>(
			[&Coordinator, &TransientTargets, &ContactVisibility,
				&FullscreenGeometry, Results](
				FRHICommandListImmediate& CommandList) {
				(*Results)[0] =
					!ContactVisibility.EnsureTargets_RenderThread(64, 32);
				(*Results)[1] =
					!ContactVisibility.EnsureTargets_RenderThread(64, 32);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{});
				auto Recovered =
					ContactVisibility.EnsureTargets_RenderThread(64, 32);
				(*Results)[2] = Recovered.has_value()
					&& Recovered->Visibility != nullptr
					&& Recovered->Visibility->GetFormat() == EPixelFormat::R8_UNORM;
				FRHITexture* RecoveredTexture = Recovered.has_value()
					? Recovered->Visibility.GetReference() : nullptr;
				auto Alternate =
					ContactVisibility.EnsureTargets_RenderThread(32, 16);
				(*Results)[3] = Alternate.has_value()
					&& Alternate->Visibility->GetSizeX() == 32
					&& Alternate->Visibility->GetSizeY() == 16;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{});
				auto DeviceTargets =
					ContactVisibility.EnsureTargets_RenderThread(64, 32);
				(*Results)[4] = DeviceTargets.has_value()
					&& DeviceTargets->Visibility.GetReference() != RecoveredTexture;
				FRHITexture* DeviceTexture = DeviceTargets.has_value()
					? DeviceTargets->Visibility.GetReference() : nullptr;
				ContactVisibility.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				auto Released =
					ContactVisibility.EnsureTargets_RenderThread(64, 32);
				(*Results)[5] = Released.has_value()
					&& Released->Visibility.GetReference() != DeviceTexture;
				(*Results)[6] = FContactShadowVisibilityRenderer::
					CalculateTargetBytes(64, 32) == 2048;
				(*Results)[7] = ContactVisibility.
					GetRetainedTargetBytes_RenderThread() == 2048;
				VulkanRHI::ArmVulkanCreateFailure(
					VulkanRHI::EVulkanCreateFailurePoint::Image);
				(*Results)[8] =
					!ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				(*Results)[9] =
					!ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{});
				auto ComputeTargets =
					ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				(*Results)[10] = ComputeTargets.has_value()
					&& ComputeTargets->Visibility != nullptr
					&& ComputeTargets->Visibility->GetFormat() == EPixelFormat::R8_UNORM
					&& ComputeTargets->SampledView != nullptr
					&& ComputeTargets->StorageView != nullptr;
				FRHITexture* ComputeTexture = ComputeTargets.has_value()
					? ComputeTargets->Visibility.GetReference() : nullptr;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{});
				auto RecreatedCompute =
					ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				(*Results)[11] = RecreatedCompute.has_value()
					&& RecreatedCompute->Visibility.GetReference() != ComputeTexture;
				FRHITexture* RecreatedComputeTexture = RecreatedCompute.has_value()
					? RecreatedCompute->Visibility.GetReference() : nullptr;
				ContactVisibility.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				auto ReleasedCompute =
					ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				(*Results)[12] = ReleasedCompute.has_value()
					&& ReleasedCompute->Visibility.GetReference()
						!= RecreatedComputeTexture;
				(*Results)[13] = FContactShadowVisibilityRenderer::
					CalculateGroupCount(65) == 9;

				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				auto MakeInput = [&CommandList](const char* Name,
					EPixelFormat Format, ETextureCreateFlags Flags) {
					return GDynamicRHI->RHICreateTexture(CommandList,
						FRHITextureCreateDesc::Create2D(Name, 65, 33, Format)
							.SetFlags(Flags));
				};
				const ETextureCreateFlags Sampled = ETextureCreateFlags::ShaderResource;
				FTextureRHIRef Material = MakeInput(
					"ContactFailureMaterial", EPixelFormat::RGBA8_UNORM, Sampled);
				FTextureRHIRef Normals = MakeInput(
					"ContactFailureNormals", EPixelFormat::RGBA8_UNORM, Sampled);
				FTextureRHIRef Surface = MakeInput(
					"ContactFailureSurface", EPixelFormat::RGBA8_UNORM, Sampled);
				FTextureRHIRef Emissive = MakeInput(
					"ContactFailureEmissive", EPixelFormat::R11G11B10_FLOAT, Sampled);
				FTextureRHIRef Depth = MakeInput("ContactFailureDepth",
					EPixelFormat::D32, ETextureCreateFlags::DepthStencilTargetable
						| ETextureCreateFlags::ShaderResource);
				ASSERT_TRUE(Material && Normals && Surface && Emissive && Depth);
				const std::array InputTransitions{
					FRHITextureTransition::Whole(Material, ERHIAccess::Discard,
						ERHIAccess::GraphicsShaderRead),
					FRHITextureTransition::Whole(Normals, ERHIAccess::Discard,
						ERHIAccess::GraphicsShaderRead),
					FRHITextureTransition::Whole(Surface, ERHIAccess::Discard,
						ERHIAccess::GraphicsShaderRead),
					FRHITextureTransition::Whole(Emissive, ERHIAccess::Discard,
						ERHIAccess::GraphicsShaderRead),
					FRHITextureTransition::Whole(Depth, ERHIAccess::Discard,
						ERHIAccess::GraphicsShaderRead)};
				CommandList.TransitionTextures(InputTransitions);
				auto FragmentTargets =
					ContactVisibility.EnsureTargets_RenderThread(65, 33);
				auto FailureComputeTargets =
					ContactVisibility.EnsureComputeTargets_RenderThread(65, 33);
				FSceneView View;
				View.ViewProjectionMatrix = FMatrix(1.0);
				View.ViewportWidth = 65;
				View.ViewportHeight = 33;
				VulkanRHI::ArmVulkanCreateFailure(
					VulkanRHI::EVulkanCreateFailurePoint::PipelineLayout);
				const auto FailedCompute = ContactVisibility.Render_RenderThread(
					CommandList, true, &*FragmentTargets, &*FailureComputeTargets,
					Material, Normals, Surface, Emissive, Depth, View,
					FVector3(0.0, 0.0, -1.0), 65, 33, {});
				(*Results)[14] = FailedCompute.Route
					== FContactShadowVisibilityRenderer::ERoute::Fragment;
				const auto SuppressedRetry = ContactVisibility.Render_RenderThread(
					CommandList, true, &*FragmentTargets, &*FailureComputeTargets,
					Material, Normals, Surface, Emissive, Depth, View,
					FVector3(0.0, 0.0, -1.0), 65, 33, {});
				(*Results)[15] = SuppressedRetry.Route
					== FContactShadowVisibilityRenderer::ERoute::Fragment;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{});
				const auto RecoveredCompute = ContactVisibility.Render_RenderThread(
					CommandList, true, &*FragmentTargets, &*FailureComputeTargets,
					Material, Normals, Surface, Emissive, Depth, View,
					FVector3(0.0, 0.0, -1.0), 65, 33, {});
				(*Results)[16] = RecoveredCompute.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute;
				(*Results)[17] = RecoveredCompute.Visibility
					== FailureComputeTargets->Visibility.GetReference();
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				ContactVisibility.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				FullscreenGeometry.ReleaseResources_RenderThread();
			});
		FlushRenderingCommands();
		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, RequiredDeferredSceneFailsWhenGBufferIsUnavailable)
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
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("RequiredDeferredFailureTest");
		RendererLifecycle.Start(Renderer);

		auto Output = std::make_shared<FTextureRHIRef>();
		auto ForwardResult = std::make_shared<ERenderViewResult>();
		auto RequiredResult = std::make_shared<ERenderViewResult>();
		const size_t CaptureCountBefore = GViewTelemetryCaptureCount;
		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&Renderer, Output, ForwardResult, RequiredResult](
				FRHICommandListImmediate& CommandList
			) {
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = FRHITextureCreateDesc::Create2D(
									  "RequiredDeferredFailureOutput", 64, 32,
									  EPixelFormat::SRGBA8_UNORM
				)
									  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
				*Output = GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(*Output, nullptr);
				FSceneView View;
				View.ViewportWidth = 64;
				View.ViewportHeight = 32;
				View.ClearColor = {0.25f, 0.5f, 1.0f, 0.75f};
				View.Settings.Mode.RenderMode = ERenderMode::Unlit;
				*ForwardResult = Renderer.RenderView(
					CommandList, nullptr, View, *Output, false, {}
				);

				View.Settings.Mode.RenderMode = ERenderMode::Lit;
				VulkanRHI::ArmVulkanCreateFailure(
					VulkanRHI::EVulkanCreateFailurePoint::Image
				);
				SetViewRenderTelemetrySink(CaptureViewTelemetry);
				*RequiredResult = Renderer.RenderView(
					CommandList, nullptr, View, *Output, false, {}
				);
				SetViewRenderTelemetrySink(nullptr);
				VulkanRHI::ResetVulkanCreateFailures();
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			}
		);
		FlushRenderingCommands();

		EXPECT_EQ(*ForwardResult, ERenderViewResult::Success);
		EXPECT_EQ(*RequiredResult, ERenderViewResult::RendererResourcesUnavailable);
		EXPECT_EQ(GViewTelemetryCaptureCount, CaptureCountBefore);

		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[Output](FRHICommandListImmediate&) { *Output = nullptr; }
		);
		FlushRenderingCommands();
		RendererLifecycle.Shutdown();
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, DeferredDirectionalResourcesRecoverAcrossFailureAndGenerationChanges)
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
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FRendererTransientTargetPool TransientTargets(Coordinator);
		FFullscreenGeometryResources FullscreenGeometry;
		FDeferredDirectionalLightingRenderer Deferred(
			Coordinator, FullscreenGeometry, TransientTargets
		);
		auto Results = std::make_shared<std::array<bool, 16>>();
		auto FirstTarget = std::make_shared<
			FDeferredDirectionalLightingRenderer::FTargets>();
		auto AlternateTarget = std::make_shared<
			FDeferredDirectionalLightingRenderer::FTargets>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::Image
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::ShaderModule
		);
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::GraphicsPipeline
		);
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Coordinator, &TransientTargets, &FullscreenGeometry, &Deferred, Results,
			 FirstTarget, AlternateTarget](FRHICommandListImmediate& CommandList) {
				(*Results)[0] =
					!Deferred.EnsureTargets_RenderThread(64, 32);
				(*Results)[1] =
					!Deferred.EnsureTargets_RenderThread(64, 32);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto Recovered =
					Deferred.EnsureTargets_RenderThread(64, 32);
				(*Results)[2] = Recovered.has_value();
				if (Recovered.has_value()) *FirstTarget = *Recovered;
				auto Alternate =
					Deferred.EnsureTargets_RenderThread(32, 16);
				(*Results)[3] = Alternate.has_value();
				if (Alternate.has_value()) *AlternateTarget = *Alternate;

				(*Results)[4] = !Deferred.EnsureResources_RenderThread(CommandList);
				(*Results)[5] = !Deferred.EnsureResources_RenderThread(CommandList);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*Results)[6] = !Deferred.EnsureResources_RenderThread(CommandList);
				(*Results)[7] = !Deferred.EnsureResources_RenderThread(CommandList);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				(*Results)[8] = Deferred.EnsureResources_RenderThread(CommandList);
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ShaderChanged,
					FRendererResourceInvalidationTargets{}
				);
				(*Results)[9] = Deferred.EnsureResources_RenderThread(CommandList);

				FRHITexture* BeforeDevice = FirstTarget->Color.GetReference();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{}
				);
				auto DeviceTarget =
					Deferred.EnsureTargets_RenderThread(64, 32);
				(*Results)[10] = DeviceTarget.has_value()
								 && DeviceTarget->Color.GetReference() != BeforeDevice;
				(*Results)[11] =
					Deferred.EnsureResources_RenderThread(CommandList);

				FRHITexture* BeforeRelease = DeviceTarget.has_value() ? DeviceTarget->Color.GetReference() : nullptr;
				Deferred.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				FullscreenGeometry.ReleaseResources_RenderThread();
				auto ReleasedTarget =
					Deferred.EnsureTargets_RenderThread(64, 32);
				(*Results)[12] = ReleasedTarget.has_value()
								 && ReleasedTarget->Color.GetReference() != BeforeRelease;
				(*Results)[13] =
					Deferred.EnsureResources_RenderThread(CommandList);
				(*Results)[14] = ReleasedTarget.has_value()
								 && ReleasedTarget->Color->GetFormat()
										== EPixelFormat::RGBA16_FLOAT;
				(*Results)[15] = AlternateTarget->Color != FirstTarget->Color;
			}
		);
		FlushRenderingCommands();

		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;
		ASSERT_NE(FirstTarget->Color, nullptr);
		EXPECT_EQ(FirstTarget->Color->GetSizeX(), 64u);
		EXPECT_EQ(FirstTarget->Color->GetSizeY(), 32u);
		ASSERT_NE(AlternateTarget->Color, nullptr);
		EXPECT_EQ(AlternateTarget->Color->GetSizeX(), 32u);
		EXPECT_EQ(AlternateTarget->Color->GetSizeY(), 16u);

		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&TransientTargets, &Deferred, &FullscreenGeometry,
				FirstTarget, AlternateTarget](
				FRHICommandListImmediate&
			) {
				Deferred.ReleaseResources_RenderThread();
				TransientTargets.Release_RenderThread();
				FullscreenGeometry.ReleaseResources_RenderThread();
				*FirstTarget = {};
				*AlternateTarget = {};
			}
		);
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests, WindowBackedPresentPreservesDisplaySettingsAcrossResizeAndToggles)
	{
		class FTestApplication final : public FGenericApplication
		{
		public:
			explicit FTestApplication(std::shared_ptr<FGenericWindow> InWindow)
				: Window(std::move(InWindow))
			{
			}

			auto FindWindowByNativeWindowHandle(void* NativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				return Window != nullptr
							   && Window->GetOSNativeWindowHandle() == NativeWindowHandle ?
						   Window :
						   nullptr;
			}

			std::shared_ptr<FGenericWindow> Window;
		};

		InitializeApplicationCore();
		auto Window = MakePlatformWindow();
		auto Definition = std::make_shared<FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 0.0f;
		Definition->YDesiredPositionOnScreen = 0.0f;
		Definition->WidthDesiredOnScreen = 96.0f;
		Definition->HeightDesiredOnScreen = 64.0f;
		Definition->Title = "HDR display mapping present validation";
		Window->Initialize(Definition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(Window);

		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(FRHIInitializationContext::Headless());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("HDRDisplayMappingPresentTest");
		RendererLifecycle.Start(Renderer);
		FSceneViewStateOwner ViewStateOwner = Renderer.CreateViewState();
		ASSERT_TRUE(ViewStateOwner);
		const FSceneViewStateId ViewStateId = ViewStateOwner.GetId();

		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport({
			.NativeWindowHandle = Window->GetOSNativeWindowHandle(),
			.SizeX = 96,
			.SizeY = 64,
			.PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM,
			.PresentModePolicy = EViewportPresentModePolicy::MainWindow});
		ASSERT_NE(Viewport, nullptr);
		auto RenderPresent = [&Renderer, &Viewport, ViewStateId](
								 uint32 Width,
								 uint32 Height,
								 float ExposureEV,
								 bool bEnableFXAA,
								 bool bEnableContactShadows,
								 bool bEditorAssistance,
								 bool bGBufferDebug = false
							 ) {
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[&Renderer, Viewport, Width, Height, ExposureEV,
				 bEnableFXAA, bEnableContactShadows,
				 bEditorAssistance, bGBufferDebug, ViewStateId, Result](
					FRHICommandListImmediate& CommandList
				) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					CommandList.BeginDrawingViewport(Viewport, nullptr);
					FTextureRHIRef BackBuffer =
						GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
					ASSERT_NE(BackBuffer, nullptr);
					FSceneView View = bEditorAssistance ? MakeGridView({1.0, 1.0, -0.5}) : FSceneView{};
					View.ViewStateId = ViewStateId;
					View.ViewportWidth = Width;
					View.ViewportHeight = Height;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.PostProcess.ExposureEV = ExposureEV;
					View.Settings.PostProcess.bEnableFXAA = bEnableFXAA;
					View.Settings.DirectionalShadow.bEnableContactShadows =
						bEnableContactShadows;
					FSceneViewRenderOptions Options;
					Options.GBufferDebugMode = bGBufferDebug ? EGBufferDebugMode::ReconstructionError : EGBufferDebugMode::Disabled;
					FScopedRendererQualificationPolicy Qualification({
						.bEnableDeferredDirectional = bGBufferDebug});
					*Result = Renderer.RenderView(
						CommandList, nullptr, View, BackBuffer, true, Options
					);
					CommandList.EndDrawingViewport(Viewport, true, false);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			FlushRenderingCommands();
			return *Result;
		};

		EXPECT_EQ(RenderPresent(96, 64, 0.0f, false, false, false), ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 128, 72, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(128, 72, -2.0f, true, true, false), ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 96, 64, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(96, 64, 1.0f, false, false, false), ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 129, 129, false);
		FlushRenderingCommands();
		SetViewRenderTelemetrySink(CaptureViewTelemetry);
		EXPECT_EQ(RenderPresent(129, 129, 0.0f, true, false, false), ERenderViewResult::Success);
		EXPECT_EQ(GLastViewTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
		EXPECT_EQ(GLastViewTelemetry.Deferred.HybridDeferredUnavailableViews, 0u);
		EXPECT_EQ(RenderPresent(129, 129, 0.0f, true, false, true, true), ERenderViewResult::Success);
		EXPECT_EQ(GLastViewTelemetry.Deferred.DeferredDirectionalEnabledViews, 1u);
		EXPECT_EQ(GLastViewTelemetry.Deferred.DeferredDirectionalUnavailableViews, 0u);
		EXPECT_EQ(GLastViewTelemetry.Deferred.DeferredDirectionalPassFailures, 0u);
		EXPECT_GT(GLastViewTelemetry.Deferred.DeferredDirectionalOutputBytes, 0u);
		EXPECT_EQ(GLastViewTelemetry.GBuffer.GBufferAttachmentBytes, GLastViewTelemetry.Deferred.DeferredDirectionalOutputBytes * 2u);
		SetViewRenderTelemetrySink(nullptr);

		Viewport = nullptr;
		ViewStateOwner.Reset();
		FlushRenderingCommands();
		RendererLifecycle.Shutdown();
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
		GApp = nullptr;
		Window.reset();
		ShutdownApplicationCore();
	}
} // namespace Durin
