#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "Console/ConsoleCommand.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Resources/RenderTargetLayouts.h"
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

		auto CapturePostProcessTiming(
			const FGPUTimingQueryRHIRef& Query) -> void
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

		auto BuildViewMatrix(
			const FVector3& Location,
			const FVector3& Forward) -> FMatrix
		{
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
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
				View.ProjectionMatrix));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = 129;
			View.ViewportHeight = 129;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.0f, 0.0f, 0.0f, 1.0f};
			View.Settings.RenderMode = ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = EViewLODMode::ForceLOD0;
			View.EditorGrid.bVisible = true;
			View.EditorGrid.Height = 0.0;
			View.EditorGrid.FadeDistance = 1000.0f;
			View.EditorGrid.MinorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.MajorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.AxisXColor = {1.0f, 0.0f, 0.0f, 1.0f};
			View.EditorGrid.AxisYColor = {0.0f, 1.0f, 0.0f, 1.0f};
			return View;
		}

		auto CountVisiblePixels(const std::vector<uint8>& Pixels) -> size_t
		{
			size_t Result = 0;
			for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
			{
				Result += Pixels[Offset] > 16
					|| Pixels[Offset + 1] > 16
					|| Pixels[Offset + 2] > 16 ? 1u : 0u;
			}
			return Result;
		}

		auto RenderGridCapture(
			FRendererModule& Renderer,
			FScene* Scene,
			const FVector3& Forward) -> std::vector<uint8>
		{
			auto Pixels = std::make_shared<std::vector<uint8>>();
			EnqueueRenderCommand<FRenderEditorGridCapture>(
				[&Renderer, Scene, Forward, Pixels](
					FRHICommandListImmediate& CommandList) {
					GRenderFrameCounterRenderThread++;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
						"EditorGridValidationColor", 129, 129,
						EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Target =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);

					const FSceneView View = MakeGridView(Forward);
					EXPECT_EQ(Renderer.RenderView(
						CommandList, Scene, View, Target, false, {}),
						ERenderViewResult::Success);
					ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Target, 0, 0, *Pixels));
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
			return std::move(*Pixels);
		}
	}

	TEST(FEditorGridVulkanTests, ReversedZGridRemainsStableAcrossCoplanarRotatedViews)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit();
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		Durin::FModuleTestHarness RendererLifecycle("EditorGridRendererTest");
		RendererLifecycle.Start(Renderer);

		auto FailureResults = std::make_shared<
			std::array<ERenderViewResult, 2>>();
		auto FailurePixelsBefore = std::make_shared<std::vector<uint8>>();
		auto FailurePixelsAfter = std::make_shared<std::vector<uint8>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::GraphicsPipeline);
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Renderer, FailureResults, FailurePixelsBefore, FailurePixelsAfter](
				FRHICommandListImmediate& CommandList) {
				GRenderFrameCounterRenderThread++;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto Desc = FRHITextureCreateDesc::Create2D(
					"FailedDisplayPayloadOutput", 1, 1,
					EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::CPUReadback);
				FTextureRHIRef Output =
					GDynamicRHI->RHICreateTexture(CommandList, Desc);
				ASSERT_NE(Output, nullptr);
				FRHIRenderPassInfo SentinelPass{};
				SentinelPass.RenderTargetLayout =
					RenderTargetLayouts::MakeFinalScenePostProcessOutput(
						RenderTargetLayouts::EViewportOutput::Offscreen);
				SentinelPass.ColorRenderTargets[0] = Output;
				SentinelPass.ColorClearValues[0] =
					FClearValueBinding(0.25f, 0.5f, 0.75f, 1.0f);
				CommandList.BeginRenderPass(
					SentinelPass, "FailedDisplayPayloadSentinelPass");
				CommandList.EndRenderPass();
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, Output, 0, 0, *FailurePixelsBefore));
				FSceneView View;
				View.ClearColor = {4.0f, 2.0f, 1.0f, 1.0f};
				View.Settings.bEnableFXAA = false;
				(*FailureResults)[0] = Renderer.RenderView(
					CommandList, nullptr, View, Output, false, {});
				(*FailureResults)[1] = Renderer.RenderView(
					CommandList, nullptr, View, Output, false, {});
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, Output, 0, 0, *FailurePixelsAfter));
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		FlushRenderingCommands();
		EXPECT_EQ((*FailureResults)[0],
			ERenderViewResult::RendererResourcesUnavailable);
		EXPECT_EQ((*FailureResults)[1],
			ERenderViewResult::RendererResourcesUnavailable);
		EXPECT_EQ(*FailurePixelsAfter, *FailurePixelsBefore);
		EXPECT_TRUE(FConsoleCommandRegistry::Get().Execute(
			"renderer.retry-resources").bSuccess);
		FlushRenderingCommands();

		auto HDRPixels = std::make_shared<std::vector<uint8>>();
		auto DefaultExposurePixels = std::make_shared<std::vector<uint8>>();
		auto LowExposurePixels = std::make_shared<std::vector<uint8>>();
		std::vector<FGPUTimingQueryRHIRef> PostProcessTimingQueries;
		GPostProcessTimingQueries = &PostProcessTimingQueries;
		SetPostProcessTimingQuerySink(CapturePostProcessTiming);
		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&Renderer, HDRPixels, DefaultExposurePixels, LowExposurePixels](
				FRHICommandListImmediate& CommandList) {
				GRenderFrameCounterRenderThread++;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				const auto HDRDesc = FRHITextureCreateDesc::Create2D(
					"HDRPreservationValidation", 1, 1,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::CPUReadback);
				FTextureRHIRef HDRTarget =
					GDynamicRHI->RHICreateTexture(CommandList, HDRDesc);
				ASSERT_NE(HDRTarget, nullptr);
				FRHIRenderPassInfo HDRPass{};
				HDRPass.RenderTargetLayout =
					RenderTargetLayouts::MakeContactShadowOutput();
				HDRPass.ColorRenderTargets[0] = HDRTarget;
				HDRPass.ColorClearValues[0] =
					FClearValueBinding(4.0f, 2.0f, 0.5f, 0.5f);
				CommandList.BeginRenderPass(HDRPass, "HDRPreservationValidationPass");
				CommandList.EndRenderPass();
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
					CommandList, HDRTarget, 0, 0, *HDRPixels));

				auto CaptureDisplay = [&](const char* Name, float ExposureEV,
					std::vector<uint8>& Pixels) {
					const auto OutputDesc = FRHITextureCreateDesc::Create2D(
						Name, 1, 1, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Output =
						GDynamicRHI->RHICreateTexture(CommandList, OutputDesc);
					EXPECT_NE(Output, nullptr);
					FSceneView View;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.bEnableFXAA = false;
					View.Settings.ExposureEV = ExposureEV;
					EXPECT_EQ(Renderer.RenderView(
						CommandList, nullptr, View, Output, false, {}),
						ERenderViewResult::Success);
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, Pixels));
				};
				CaptureDisplay(
					"DefaultExposureValidation", 0.0f,
					*DefaultExposurePixels);
				CaptureDisplay(
					"LowExposureValidation", -2.0f,
					*LowExposurePixels);
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
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
					});
			if (bReady) break;
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[](FRHICommandListImmediate& CommandList) {
					GRenderFrameCounterRenderThread++;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
		}
		ASSERT_EQ(HDRPixels->size(), 8u);
		auto ReadHalfBits = [&HDRPixels](size_t Channel) {
			const size_t Offset = Channel * 2;
			return static_cast<uint16>((*HDRPixels)[Offset])
				| static_cast<uint16>((*HDRPixels)[Offset + 1] << 8);
		};
		EXPECT_EQ(ReadHalfBits(0), 0x4400u); // 4.0
		EXPECT_EQ(ReadHalfBits(1), 0x4000u); // 2.0
		EXPECT_EQ(ReadHalfBits(2), 0x3800u); // 0.5
		EXPECT_EQ(ReadHalfBits(3), 0x3800u); // alpha 0.5
		ASSERT_EQ(DefaultExposurePixels->size(), 4u);
		ASSERT_EQ(LowExposurePixels->size(), 4u);
		EXPECT_GT((*DefaultExposurePixels)[0], (*LowExposurePixels)[0] + 10u);
		EXPECT_GT((*DefaultExposurePixels)[0], 248u);
		EXPECT_GT((*DefaultExposurePixels)[1], (*LowExposurePixels)[1]);
		EXPECT_NEAR((*DefaultExposurePixels)[3], 128u, 1u);
		EXPECT_NEAR((*LowExposurePixels)[3], 128u, 1u);
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
			FViewRouteCase{"AssetThumbnail", 96, 96, 0.0f, true}};
		auto CaptureViewRoute = [&Renderer](const FViewRouteCase& Route) {
			auto Pixels = std::make_shared<std::vector<uint8>>();
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[&Renderer, Route, Pixels](
					FRHICommandListImmediate& CommandList) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
						Route.Name,
						Route.Width,
						Route.Height,
						EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Output =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Output, nullptr);
					FSceneView View;
					View.ViewportWidth = Route.Width;
					View.ViewportHeight = Route.Height;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.ExposureEV = Route.ExposureEV;
					View.Settings.bEnableFXAA = Route.bEnableFXAA;
					EXPECT_EQ(Renderer.RenderView(
						CommandList, nullptr, View, Output, false, {}),
						ERenderViewResult::Success);
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, *Pixels));
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
			return std::move(*Pixels);
		};
		std::array<std::vector<uint8>, 4> RoutePixels;
		for (size_t Index = 0; Index < ViewRouteCases.size(); ++Index)
		{
			RoutePixels[Index] = CaptureViewRoute(ViewRouteCases[Index]);
			EXPECT_EQ(RoutePixels[Index].size(),
				static_cast<size_t>(ViewRouteCases[Index].Width)
					* ViewRouteCases[Index].Height * 4u);
			ASSERT_GE(RoutePixels[Index].size(), 4u);
			EXPECT_NEAR(RoutePixels[Index][3], 128u, 1u);
		}
		EXPECT_LT(RoutePixels[1][0], RoutePixels[0][0]);
		EXPECT_GT(RoutePixels[2][2], RoutePixels[0][2]);
		EXPECT_EQ(RoutePixels[3][0], RoutePixels[0][0]);
		EXPECT_EQ(RoutePixels[3][1], RoutePixels[0][1]);
		EXPECT_EQ(RoutePixels[3][2], RoutePixels[0][2]);
		const std::vector<uint8> MainAfterOtherViews =
			CaptureViewRoute(ViewRouteCases.front());
		EXPECT_EQ(MainAfterOtherViews, RoutePixels.front());

		const std::array<uint16, 9> Samples{};
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		std::string Error;
		ASSERT_TRUE(BuildTerrainHeightmapPayload(
			3, 3, Samples, Payload, Error)) << Error;
		auto Material = MakeRefCount<FMaterialRenderProxy>();
		FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties = FMaterialStaticProperties{
			.BlendMode = EMaterialBlendMode::Opaque,
			.ShadingModel = EMaterialShadingModel::Unlit,
			.bTwoSided = true};
		Publication.LocalLayer.Parameters.push_back({
			.Id = MaterialParameters::BaseColorId,
			.Type = EMaterialParameterType::Vector,
			.VectorValue = {0.0f, 0.0f, 0.0f}});
		ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
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
				{0.0, 0.0, 0.0}, {TerrainExtent, TerrainExtent, 0.0})};
		FScene Scene;
		Scene.AddOrReplacePrimitive(
			FPrimitiveSceneId(1),
			std::make_unique<FTerrainSceneProxy>(Payload, 1,
				TerrainExtent * 0.5, TerrainExtent * 0.5,
				1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch},
				Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.0}));
		FScene OccluderScene;
		OccluderScene.AddOrReplacePrimitive(
			FPrimitiveSceneId(2),
			std::make_unique<FTerrainSceneProxy>(Payload, 1,
				TerrainExtent * 0.5, TerrainExtent * 0.5,
				1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch},
				Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.25}));
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
			const std::vector<uint8> EmptyPixels =
				RenderGridCapture(Renderer, nullptr, Forward);
			const std::vector<uint8> TerrainPixels =
				RenderGridCapture(Renderer, &Scene, Forward);
			ASSERT_EQ(EmptyPixels.size(), 129u * 129u * 4u);
			ASSERT_EQ(TerrainPixels.size(), EmptyPixels.size());
			const size_t EmptyVisible = CountVisiblePixels(EmptyPixels);
			const size_t TerrainVisible = CountVisiblePixels(TerrainPixels);
			ASSERT_GT(EmptyVisible, 0u);
			EXPECT_GE(TerrainVisible, EmptyVisible * 99u / 100u);
		}
		const std::vector<uint8> OccludedPixels = RenderGridCapture(
			Renderer, &OccluderScene, CameraDirections.front());
		EXPECT_EQ(CountVisiblePixels(OccludedPixels), 0u);

		RendererLifecycle.Shutdown();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST(FEditorGridVulkanTests,
		WindowBackedPresentPreservesDisplaySettingsAcrossResizeAndToggles)
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
					&& Window->GetOSNativeWindowHandle() == NativeWindowHandle
					? Window
					: nullptr;
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
		RHIInit();
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		auto RendererContext =
			FModuleTestContextFactory::CreateStartupContext(
				"HDRDisplayMappingPresentTest");
		Renderer.StartupModule(RendererContext);

		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport(
			Window->GetOSNativeWindowHandle(),
			96,
			64,
			false,
			EPixelFormat::SRGBA8_UNORM,
			EViewportPresentModePolicy::MainWindow);
		ASSERT_NE(Viewport, nullptr);
		auto RenderPresent = [&Renderer, &Viewport](
			uint32 Width,
			uint32 Height,
			float ExposureEV,
			bool bEnableFXAA,
			bool bEnableContactShadows,
			bool bEditorAssistance) {
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable);
			EnqueueRenderCommand<FCaptureHDRDisplayContract>(
				[&Renderer, Viewport, Width, Height, ExposureEV,
					bEnableFXAA, bEnableContactShadows,
					bEditorAssistance, Result](
					FRHICommandListImmediate& CommandList) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					CommandList.BeginDrawingViewport(Viewport, nullptr);
					FTextureRHIRef BackBuffer =
						GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
					ASSERT_NE(BackBuffer, nullptr);
					FSceneView View = bEditorAssistance
						? MakeGridView({1.0, 1.0, -0.5})
						: FSceneView{};
					View.ViewportWidth = Width;
					View.ViewportHeight = Height;
					View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
					View.Settings.ExposureEV = ExposureEV;
					View.Settings.bEnableFXAA = bEnableFXAA;
					View.Settings.bEnableContactShadows =
						bEnableContactShadows;
					*Result = Renderer.RenderView(
						CommandList, nullptr, View, BackBuffer, true, {});
					CommandList.EndDrawingViewport(Viewport, true, false);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
			return *Result;
		};

		EXPECT_EQ(RenderPresent(96, 64, 0.0f, false, false, false),
			ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 128, 72, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(128, 72, -2.0f, true, true, false),
			ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 96, 64, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(96, 64, 1.0f, false, false, false),
			ERenderViewResult::Success);
		GDynamicRHI->RHIResizeViewport(Viewport, 129, 129, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(129, 129, 0.0f, true, false, true),
			ERenderViewResult::Success);

		Viewport = nullptr;
		auto RendererShutdownContext =
			FModuleTestContextFactory::CreateShutdownContext(RendererContext);
		Renderer.ShutdownModule(RendererShutdownContext);
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
		GApp = nullptr;
		Window.reset();
		ShutdownApplicationCore();
	}
} // namespace Durin
