#include "Materials/MaterialResourceTestSupport.h"
#include "../../RDGTestAccess.h"

#include "RDG/RDG.h"
#include <gtest/gtest.h>
#include "VulkanEngineTestSupport.h"
#include "NativeDObjectTestSupport.h"

#include "CoreGlobals.h"
#include "Console/ConsoleCommand.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/PrimitiveDrawInterface.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Threading/TaskRuntime.h"
#include "RendererModule.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "RendererFeatureTargetTestFixture.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "SceneTestAccess.h"
#include "SceneViewProjection.h"
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin
{
	class FEditorGridVulkanTests : public testing::Test
	{
	public:
		auto SetUp() -> void override
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
			bOwnScheduler = !IsTaskSchedulerRunning();
			if (bOwnScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
		}
		auto TearDown() -> void override
		{
			if (bOwnScheduler) ShutdownTaskScheduler();
		}
		static auto SetUpTestSuite() -> void
		{
			Testing::InitializeDObjectSystemForTests();
			// Engine owns the global shader-map release hook for each RHI lifetime.
			FModuleManager::Get().LoadModuleChecked("Engine");
		}
	private:
		bool bOwnScheduler = false;
	};

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

		class FValidationFrame final
		{
		public:
			explicit FValidationFrame(FRHICommandListImmediate& InCommands) : Commands(InCommands)
			{
				++GRenderFrameCounterRenderThread;
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			}
			~FValidationFrame() { GDynamicRHI->RHIEndFrame_RenderThread(Commands); }
		private:
			FRHICommandListImmediate& Commands;
		};

		// Only the host waits between attempts. Each render command performs one pump.
		template<class FrameBody>
		auto PumpValidationFrames(FrameBody&& Body) -> bool
		{
			bool Complete = false;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			do
			{
				EnqueueRenderCommand("EditorGridValidationFrame", [&](FRHICommandListImmediate& Commands) {
					FValidationFrame Frame(Commands);
					Complete = Body(Commands);
				});
				FlushRenderingCommands();
				if (Complete || testing::Test::HasFatalFailure()) return Complete;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < Deadline);
			ADD_FAILURE() << "EditorGrid resource preparation did not finish before its deadline.";
			return false;
		}

		auto PrepareValidationView(FRendererModule& Renderer, FRHICommandListImmediate& Commands,
			const FSceneViewRenderRequest& Request, const FRenderResourcePreparation& Preparation)
			-> ERenderResourcePreparationState
		{
			Renderer.BeginFrame_RenderThread(Commands);
			const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
			const auto TelemetryBefore = GViewTelemetryCaptureCount;
			const auto QueriesBefore = GPostProcessTimingQueries ? GPostProcessTimingQueries->size() : 0;
			Renderer.PrepareViewResources(Commands, Request, Preparation);
			EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
			EXPECT_EQ(GViewTelemetryCaptureCount, TelemetryBefore);
			if (GPostProcessTimingQueries) EXPECT_EQ(GPostProcessTimingQueries->size(), QueriesBefore);
			return Preparation.GetProgress().State;
		}

		auto RenderPreparedView(FRendererModule& Renderer, const FSceneViewRenderRequest& Request,
			const std::function<void(FRHICommandListImmediate&)>& AfterDraw,
			const FRendererQualificationPolicy& Policy = {}) -> ERenderViewResult
		{
			const auto Preparation = FRenderResourcePreparation::Create();
			auto Result = ERenderViewResult::RendererResourcesPending;
			PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
				FScopedRendererQualificationPolicy Qualification(Policy);
				const auto State = PrepareValidationView(Renderer, Commands, Request, Preparation);
				if (State == ERenderResourcePreparationState::Pending) return false;
				if (State != ERenderResourcePreparationState::Ready)
				{
					ADD_FAILURE() << "View preparation reached terminal state " << static_cast<int>(State);
					Result = ERenderViewResult::RendererResourcesUnavailable;
					return true;
				}
				Result = Renderer.RenderView(Commands, Request);
				if (Result == ERenderViewResult::Success) AfterDraw(Commands);
				return true;
			});
			if (Result == ERenderViewResult::RendererResourcesPending) Preparation.RequestCancel();
			return Result;
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

		struct FRDGDescriptorReuseContract
		{
			static constexpr auto GetName() -> const char*
			{
				return "RDGDescriptorReuseContract";
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

		auto CountVisiblePixels(const Durin::FByteBuffer& Pixels) -> size_t
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
			FXxHash64 FactoryKey
		) -> FVertexDeclarationRHIRef
		{
			FVertexDeclarationElementList Elements{};
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
			return GDynamicRHI->RHICreateVertexDeclaration(Elements);
		}

		auto RenderGridCapture(
			FRendererModule& Renderer,
			FSceneInterface* Scene,
			const FVector3& Forward,
			bool bLit = false,
			bool bSimpleElements = false
		) -> Durin::FByteBuffer
		{
			auto Pixels = std::make_shared<Durin::FByteBuffer>();
			FTextureRHIRef Target;
			FSceneView View;
			EnqueueRenderCommand<FRenderEditorGridCapture>(
				[Forward, &Target, &View, bLit, bSimpleElements](
					FRHICommandListImmediate& CommandList
				) {
					FValidationFrame Frame(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
										  "EditorGridValidationColor", 129, 129,
										  EPixelFormat::SRGBA8_UNORM
					)
										  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
					Target =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);

					View = MakeGridView(Forward);
					if (bLit) View.Settings.Mode.RenderMode = ERenderMode::Lit;
					if (bSimpleElements)
					{
						View.EditorGrid.bVisible = false;
						const FVector3 Center = View.ViewLocation
							+ Math::Normalize(Forward) * 5.0;
						FViewPrimitiveDrawInterface PDI(View);
						PDI.DrawPoint(Center, {1.0f, 0.0f, 0.0f, 1.0f}, 18.0f,
							ESceneDepthPriorityGroup::Foreground);
						PDI.DrawLine(Center - FVector3(0.5, 0.0, 0.0),
							Center + FVector3(0.5, 0.0, 0.0),
							{0.0f, 1.0f, 0.0f, 1.0f},
							ESceneDepthPriorityGroup::Foreground,
							{.WidthPixels = 4.0f});
						PDI.DrawSprite(Center + FVector3(0.0, 0.0, 0.75),
							FVector2f(24.0f), FSimpleElementTexture::EditorIconAtlas(),
							{0.0f, 0.0f}, {1.0f / 3.0f, 1.0f}, FVector4f(1.0f),
							ESceneDepthPriorityGroup::Foreground);
						PDI.Seal();
					}
				}
			);
			FlushRenderingCommands();
			EXPECT_EQ(RenderPreparedView(Renderer, {Scene, View, Target, false, {}},
				[&](FRHICommandListImmediate& Commands) {
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, *Pixels));
				}), ERenderViewResult::Success);
			return std::move(*Pixels);
		}
	} // namespace

	TEST_F(FEditorGridVulkanTests, ReversedZGridRemainsStableAcrossCoplanarRotatedViews)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		Durin::FModuleTestHarness RendererLifecycle("EditorGridRendererTest");
		RendererLifecycle.Start(Renderer);
		auto Scene = Renderer.CreateScene();

		auto HDRPixels = std::make_shared<Durin::FByteBuffer>();
		auto DefaultExposurePixels = std::make_shared<Durin::FByteBuffer>();
		auto LowExposurePixels = std::make_shared<Durin::FByteBuffer>();
		std::vector<FGPUTimingQueryRHIRef> PostProcessTimingQueries;
		GPostProcessTimingQueries = &PostProcessTimingQueries;
		SetPostProcessTimingQuerySink(CapturePostProcessTiming);
		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[HDRPixels](
				FRHICommandListImmediate& CommandList
			) {
				FValidationFrame Frame(CommandList);
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
			}
		);
		FlushRenderingCommands();
		auto CaptureDisplay = [&](const char* Name, float ExposureEV, FByteBuffer& Pixels) {
			FTextureRHIRef Output;
			EnqueueRenderCommand("CreateDisplayValidationOutput", [&](FRHICommandListImmediate& Cmd) {
				FValidationFrame Frame(Cmd);
				Output = GDynamicRHI->RHICreateTexture(Cmd,
					FRHITextureCreateDesc::Create2D(Name, 1, 1, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
			});
			FlushRenderingCommands();
			ASSERT_NE(Output, nullptr);
			FSceneView View;
			View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
			View.Settings.PostProcess.bEnableFXAA = false;
			View.Settings.PostProcess.ExposureEV = ExposureEV;
			EXPECT_EQ(RenderPreparedView(Renderer, {Scene.get(), View, Output, false, {}},
				[&](FRHICommandListImmediate& Cmd) {
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd, Output, 0, 0, Pixels));
				}), ERenderViewResult::Success);
		};
		CaptureDisplay("DefaultExposureValidation", 0.0f, *DefaultExposurePixels);
		CaptureDisplay("LowExposureValidation", -2.0f, *LowExposurePixels);
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
		auto CaptureViewRoute = [&Renderer, &Scene](const FViewRouteCase& Route, bool bGBufferDebug = false) {
			FTextureRHIRef Output;
			FByteBuffer Pixels;
			EnqueueRenderCommand("CreateViewRouteOutput", [&](FRHICommandListImmediate& Cmd) {
				FValidationFrame Frame(Cmd);
				Output = GDynamicRHI->RHICreateTexture(Cmd,
					FRHITextureCreateDesc::Create2D(Route.Name, Route.Width, Route.Height, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
			});
			FlushRenderingCommands();
			FSceneView View;
			View.ViewportWidth = Route.Width;
			View.ViewportHeight = Route.Height;
			View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
			View.Settings.PostProcess.ExposureEV = Route.ExposureEV;
			View.Settings.PostProcess.bEnableFXAA = Route.bEnableFXAA;
			FSceneViewRenderOptions Options;
			Options.GBufferDebugMode = bGBufferDebug ? EGBufferDebugMode::Flags : EGBufferDebugMode::Disabled;
			EXPECT_EQ(RenderPreparedView(Renderer, {Scene.get(), View, Output, false, Options},
				[&](FRHICommandListImmediate& Cmd) {
					EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd, Output, 0, 0, Pixels));
				}, {.bEnableDeferredDirectional = bGBufferDebug}), ERenderViewResult::Success);
			return Pixels;
		};
		SetViewRenderTelemetrySink(CaptureViewTelemetry);
		std::array<Durin::FByteBuffer, 4> RoutePixels;
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
		const Durin::FByteBuffer MainAfterOtherViews =
			CaptureViewRoute(ViewRouteCases.front());
		EXPECT_EQ(MainAfterOtherViews, RoutePixels.front());
		const std::array<size_t, 5> GBufferRouteOrder{3u, 1u, 2u, 0u, 3u};
		Durin::FByteBuffer FirstThumbnailDebug;
		for (size_t OrderIndex = 0;
			 OrderIndex < GBufferRouteOrder.size(); ++OrderIndex)
		{
			const FViewRouteCase& Route =
				ViewRouteCases[GBufferRouteOrder[OrderIndex]];
			const Durin::FByteBuffer DebugPixels =
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

		const FVector3 SimpleElementForward{1.0, 1.0, -0.5};
		const Durin::FByteBuffer SimpleElementPixels = RenderGridCapture(
			Renderer, Scene.get(), SimpleElementForward, false, true);
		ASSERT_EQ(SimpleElementPixels.size(), 129u * 129u * 4u);
		EXPECT_GT(CountVisiblePixels(SimpleElementPixels), 100u);
		ASSERT_TRUE(Renderer.RequestResourceInvalidation(
			ERendererResourceInvalidationCause::Device).bSuccess);
		FlushRenderingCommands();
		const Durin::FByteBuffer RecoveredSimpleElementPixels =
			RenderGridCapture(Renderer, Scene.get(), SimpleElementForward,
				false, true);
		EXPECT_EQ(RecoveredSimpleElementPixels, SimpleElementPixels);

		FSceneInterfaceTestAccess::ReleaseScene(Scene);
		RendererLifecycle.Shutdown();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST_F(FEditorGridVulkanTests, RDGAllocatorDetachesExportsAndReusesTransientDescriptors)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		auto Captures = std::make_shared<std::array<FRDGCapture, 9>>();
		EnqueueRenderCommand<FRDGDescriptorReuseContract>(
			[Captures](FRHICommandListImmediate& CommandList) {
				FRendererResourceCoordinator Coordinator;
				FRendererRDGAllocator Allocator(Coordinator);
				FTextureRHIRef ExportedTexture;
				FBufferRHIRef ExportedBuffer;
				const std::array<std::string_view, 9> Names{
					"Diagnostic.First", "Diagnostic.Export",
					"Diagnostic.AfterExport", "Diagnostic.Renamed",
					"Diagnostic.ManualRetry", "Diagnostic.ShaderReload",
					"Diagnostic.DeviceInvalidated", "Diagnostic.DeviceReused",
					"Diagnostic.Released"};
				for (size_t Index = 0; Index < Names.size(); ++Index)
				{
					if (Index == 4 || Index == 5 || Index == 6)
					{
						// Omit release callbacks to exercise allocator-owned protection.
						const auto Cause = Index == 4
							? ERendererResourceInvalidationCause::ManualRetry
							: Index == 5
								? ERendererResourceInvalidationCause::ShaderChanged
								: ERendererResourceInvalidationCause::Device;
						Coordinator.Apply_RenderThread(Cause, {});
					}
					if (Index == 8) Allocator.Release_RenderThread();
					FRDGBuilder Builder;
					const auto Texture = Builder.CreateTexture(
						FRDGTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[Index].data(), 32, 16,
								EPixelFormat::RGBA8_UNORM)
								.SetFlags(ETextureCreateFlags::RenderTargetable)},
						Names[Index]);
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder,
						"Write", ERDGPassType::Graphics);
					FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture,
						{ERHITextureAspect::Color, 0, 1, 0, 1},
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store);
					const auto Buffer = Builder.CreateBuffer(
						FRDGBufferDesc{.Buffer = FRHIBufferDesc(
							64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
					const auto Compute = FRDGBuilderTestAccessor::AddPass(Builder, "Compute", ERDGPassType::Compute);
					FRDGBuilderTestAccessor::UseBuffer(Builder, Compute, Buffer, 0, 64, ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true);
					if (Index == 1)
					{
						Builder.QueueTextureExtraction(Texture, &ExportedTexture,
							ERHIAccess::ColorAttachmentReadWrite);
						Builder.QueueBufferExtraction(Buffer, &ExportedBuffer,
							ERHIAccess::ComputeShaderReadWrite);
					}

					const auto Result = Builder.Execute(CommandList, &Allocator);
					ASSERT_TRUE(Result.has_value()) << ToString(Result.error());
					(*Captures)[Index] = Builder.Capture();
				}
				Allocator.Release_RenderThread();
			});
		FlushRenderingCommands();
		for (const auto& Capture : *Captures)
		{
			ASSERT_EQ(Capture.Resources.size(), 2u);
			EXPECT_NE(Capture.Resources[0].PhysicalAllocationId, 0u);
		}
		EXPECT_EQ((*Captures)[0].Resources[0].AllocationDisposition, "reuse-miss");
		EXPECT_EQ((*Captures)[1].Resources[0].AllocationDisposition, "reuse-hit");
		EXPECT_EQ((*Captures)[0].Resources[0].PhysicalAllocationId,
			(*Captures)[1].Resources[0].PhysicalAllocationId);

		EXPECT_EQ((*Captures)[1].AllocationStatistics.RetainedResources, 0u);
		EXPECT_EQ((*Captures)[1].AllocationStatistics.RetainedBytes, 0u);
		for (size_t Resource = 0; Resource < 2; ++Resource)
		{
			EXPECT_EQ((*Captures)[0].Resources[Resource].PhysicalAllocationId,
				(*Captures)[1].Resources[Resource].PhysicalAllocationId);
			EXPECT_NE((*Captures)[1].Resources[Resource].PhysicalAllocationId,
				(*Captures)[2].Resources[Resource].PhysicalAllocationId);
			EXPECT_EQ((*Captures)[2].Resources[Resource].AllocationDisposition, "reuse-miss");
			EXPECT_EQ((*Captures)[3].Resources[Resource].AllocationDisposition, "reuse-hit");
			EXPECT_EQ((*Captures)[2].Resources[Resource].PhysicalAllocationId,
				(*Captures)[3].Resources[Resource].PhysicalAllocationId);
			for (size_t Index : {4u, 5u, 7u})
			{
				EXPECT_EQ((*Captures)[Index].Resources[Resource].AllocationDisposition,
					"reuse-hit");
				EXPECT_EQ((*Captures)[Index].Resources[Resource].PhysicalAllocationId,
					(*Captures)[Index - 1].Resources[Resource].PhysicalAllocationId);
			}
			for (size_t Index : {6u, 8u})
				EXPECT_EQ((*Captures)[Index].Resources[Resource].AllocationDisposition,
					"reuse-miss");
		}
		for (size_t Index = 2; Index < Captures->size(); ++Index)
		{
			EXPECT_EQ((*Captures)[Index].AllocationStatistics.RetainedResources, 2u);
			EXPECT_EQ((*Captures)[Index].AllocationStatistics.RetainedBytes,
				(*Captures)[0].AllocationStatistics.RetainedBytes);
		}

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST_F(FEditorGridVulkanTests, GBufferTargetsCreateAndRecreateAcrossInvalidation)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FGBufferRenderer GBuffer(Coordinator);
		auto* MaterialObject = NewObject<DMaterial>(
			nullptr, "GBufferRecoveryMaterial");
		const FMaterialRenderData MaterialData = Testing::CaptureMaterialRenderData(*MaterialObject);
		ASSERT_TRUE(MaterialData.CompiledProgram);
		auto RecoveredTargets =
			std::make_shared<FGBufferRenderer::FTargets>();
		auto AlternateTargets =
			std::make_shared<FGBufferRenderer::FTargets>();
		auto PipelineResults =
			std::make_shared<std::array<bool, 8>>();
		std::array<FGBufferRenderer::FPipelineRequest, 2> Requests;
		std::optional<FGBufferRenderer::FTargets> DeviceTargets, ReleasedTargets;
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Coordinator, &GBuffer,
			 RecoveredTargets, AlternateTargets, PipelineResults, MaterialData, &Requests](
				FRHICommandListImmediate&
			) {
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto Recovered =
					Tests::FRendererFeatureTargetFixture::CreateGBuffer(64, 32);
				ASSERT_TRUE(Recovered);
				*RecoveredTargets = *Recovered;
				auto Alternate =
					Tests::FRendererFeatureTargetFixture::CreateGBuffer(32, 16);
				ASSERT_TRUE(Alternate);
				*AlternateTargets = *Alternate;

				FRHIDepthStencilState Depth;
				Depth.bEnableTest = true;
				Depth.bEnableWrite = true;
				Depth.CompareOp = ERHIDepthCompareOp::Greater;
				const std::array<FXxHash64, 2> Domains{
					FStaticMeshBatchBinding{}.GetFactoryKey(),
					FSplineMeshBatchBinding{}.GetFactoryKey()
				};
				std::array<FVertexDeclarationRHIRef, 2> Declarations;
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
						.FactoryKey = Domains[Index],
						.LayoutKey = Index == 0 ? FStaticMeshBatchBinding{}.GetLayoutKey() : FSplineMeshBatchBinding{}.GetLayoutKey()
					};
				};
				Requests = {MakeRequest(0), MakeRequest(1)};
			});
		FlushRenderingCommands();
		auto PreparePipeline = [&](size_t RequestIndex, size_t ResultIndex,
			std::optional<ERendererResourceInvalidationCause> Cause = {}) {
			bool bFirstFrame = true;
			(*PipelineResults)[ResultIndex] = PumpValidationFrames([&](FRHICommandListImmediate&) {
				if (bFirstFrame && Cause) Coordinator.Apply_RenderThread(*Cause, {});
				bFirstFrame = false;
				return GBuffer.EnsurePipeline_RenderThread(Requests[RequestIndex]) != nullptr;
			});
		};
		PreparePipeline(0, 0, ERendererResourceInvalidationCause::ManualRetry);
		PreparePipeline(0, 1, ERendererResourceInvalidationCause::ManualRetry);
		PreparePipeline(1, 2);
		PreparePipeline(0, 3, ERendererResourceInvalidationCause::ShaderChanged);
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			FRHITexture* BeforeDevice = RecoveredTargets->Material.GetReference();
			Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::Device, {});
			DeviceTargets = Tests::FRendererFeatureTargetFixture::CreateGBuffer(64, 32);
			(*PipelineResults)[4] = DeviceTargets && DeviceTargets->Material.GetReference() != BeforeDevice;
			return true;
		});
		PreparePipeline(0, 5);
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			FRHITexture* BeforeRelease = DeviceTargets ? DeviceTargets->Material.GetReference() : nullptr;
			GBuffer.ReleaseResources_RenderThread();
			ReleasedTargets = Tests::FRendererFeatureTargetFixture::CreateGBuffer(64, 32);
			(*PipelineResults)[6] = ReleasedTargets && ReleasedTargets->Material.GetReference() != BeforeRelease;
			return true;
		});
		PreparePipeline(0, 7);
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			Requests = {};
			DeviceTargets.reset();
			ReleasedTargets.reset();
			return true;
		});

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
			[&GBuffer, RecoveredTargets, AlternateTargets](
				FRHICommandListImmediate&
			) {
				GBuffer.ReleaseResources_RenderThread();
				*RecoveredTargets = {};
				*AlternateTargets = {};
			}
		);
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST_F(FEditorGridVulkanTests, GroundTruthAmbientOcclusionTargetsRecreateAcrossInvalidation)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FFullscreenGeometryResources FullscreenGeometry;
		FGroundTruthAmbientOcclusionRenderer AmbientOcclusion(
			Coordinator, FullscreenGeometry
		);
		auto Results = std::make_shared<std::array<bool, 10>>();
		FTextureRHIRef Normals, Surface, Depth;
		std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets> ReleasedTargets;
		FSceneView View;
		View.ViewportWidth = 64;
		View.ViewportHeight = 32;
		EnqueueRenderCommand<FFailDisplayPayloadContract>(
			[&Coordinator, &AmbientOcclusion,
				&FullscreenGeometry, Results, &Normals, &Surface, &Depth, &ReleasedTargets](
				FRHICommandListImmediate& CommandList
			) {
				FValidationFrame Frame(CommandList);
				(*Results)[0] =
					Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution).has_value();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto Recovered =
					Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[1] = Recovered.has_value() && Recovered->Raw != nullptr
								&& Recovered->Scratch != nullptr
								&& Recovered->Raw->GetFormat() == EPixelFormat::R8_UNORM;
				FRHITexture* RecoveredRaw =
					Recovered.has_value() ? Recovered->Raw.GetReference() : nullptr;
				auto Alternate =
					Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(32, 16,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[2] = Alternate.has_value() && Alternate->Raw != nullptr
								&& Alternate->Raw->GetSizeX() == 32
								&& Alternate->Raw->GetSizeY() == 16;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{}
				);
				auto DeviceTargets =
					Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[3] = DeviceTargets.has_value()
								&& DeviceTargets->Raw.GetReference() != RecoveredRaw;
				FRHITexture* DeviceRaw = DeviceTargets.has_value() ? DeviceTargets->Raw.GetReference() : nullptr;
				AmbientOcclusion.ReleaseResources_RenderThread();
				ReleasedTargets =
					Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(64, 32,
						EGroundTruthAmbientOcclusionQuality::FullResolution);
				(*Results)[4] = ReleasedTargets.has_value()
								&& ReleasedTargets->Raw.GetReference() != DeviceRaw;
				(*Results)[5] = ReleasedTargets.has_value()
								&& ReleasedTargets->Raw->GetSizeX() == 64
								&& ReleasedTargets->Raw->GetSizeY() == 32;
				(*Results)[6] =
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
				Normals = RHICreateTexture(ColorDesc);
				Surface = RHICreateTexture(ColorDesc);
				Depth = RHICreateTexture(DepthDesc);
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
				auto HalfTargets = Tests::FRendererFeatureTargetFixture::CreateAmbientOcclusion(
					65, 33,
					EGroundTruthAmbientOcclusionQuality::HalfResolution);
				(*Results)[9] = HalfTargets.has_value()
					&& HalfTargets->Raw->GetSizeX() == 33
					&& HalfTargets->Raw->GetSizeY() == 17
					&& HalfTargets->Selector != nullptr
					&& HalfTargets->Resolved->GetSizeX() == 65
					&& HalfTargets->Resolved->GetSizeY() == 33;
			}
		);
		FlushRenderingCommands();
		for (size_t ResultIndex : {7u, 8u})
		{
			bool bFirstFrame = true;
			const bool bPrepared = PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
				if (bFirstFrame) Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::ManualRetry, {});
				bFirstFrame = false;
				return AmbientOcclusion.EnsureResources_RenderThread(Commands);
			});
			PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
				(*Results)[ResultIndex] = bPrepared && ReleasedTargets && AmbientOcclusion.RenderRaw_RenderThread(
					Commands, *ReleasedTargets, Normals, Surface, Depth, View);
				return true;
			});
		}
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			AmbientOcclusion.ReleaseResources_RenderThread();
			FullscreenGeometry.ReleaseResources_RenderThread();
			ReleasedTargets.reset();
			Normals = Surface = Depth = nullptr;
			return true;
		});

		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST_F(FEditorGridVulkanTests, ContactVisibilityTargetsRecreateAcrossInvalidation)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FFullscreenGeometryResources FullscreenGeometry;
		FContactShadowVisibilityRenderer ContactVisibility(
			Coordinator, FullscreenGeometry);
		auto Results = std::make_shared<std::array<bool, 14>>();
		FTextureRHIRef Material, Normals, Surface, Emissive, Depth;
		std::optional<FContactShadowVisibilityRenderer::FTargets> FragmentTargets;
		std::optional<FContactShadowVisibilityRenderer::FComputeTargets> DispatchTargets;
		FSceneView View;
		View.ViewProjectionMatrix = FMatrix(1.0);
		View.ViewportWidth = 65;
		View.ViewportHeight = 33;
		EnqueueRenderCommand<FContactVisibilityTargetLifecycle>(
			[&Coordinator, &ContactVisibility,
				&FullscreenGeometry, Results, &Material, &Normals, &Surface, &Emissive, &Depth, &FragmentTargets, &DispatchTargets](
				FRHICommandListImmediate& CommandList) {
				(*Results)[0] =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(64, 32)
						.has_value();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{});
				auto Recovered =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(64, 32);
				(*Results)[1] = Recovered.has_value()
					&& Recovered->Visibility != nullptr
					&& Recovered->Visibility->GetFormat() == EPixelFormat::R8_UNORM;
				FRHITexture* RecoveredTexture = Recovered.has_value()
					? Recovered->Visibility.GetReference() : nullptr;
				auto Alternate =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(32, 16);
				(*Results)[2] = Alternate.has_value()
					&& Alternate->Visibility->GetSizeX() == 32
					&& Alternate->Visibility->GetSizeY() == 16;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::Device,
					FRendererResourceInvalidationTargets{});
				auto DeviceTargets =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(64, 32);
				(*Results)[3] = DeviceTargets.has_value()
					&& DeviceTargets->Visibility.GetReference() != RecoveredTexture;
				FRHITexture* DeviceTexture = DeviceTargets.has_value()
					? DeviceTargets->Visibility.GetReference() : nullptr;
				ContactVisibility.ReleaseResources_RenderThread();
				auto Released =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(64, 32);
				(*Results)[4] = Released.has_value()
					&& Released->Visibility.GetReference() != DeviceTexture;
				(*Results)[5] = FContactShadowVisibilityRenderer::
					CalculateTargetBytes(64, 32) == 2048;
				(*Results)[6] =
					FContactShadowVisibilityRenderer::DescribeFragmentTarget(64, 32)
						.Format == EPixelFormat::R8_UNORM;
				(*Results)[7] =
					Tests::FRendererFeatureTargetFixture::CreateContactCompute(65, 33)
						.has_value();
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{});
				auto ComputeTargets =
					Tests::FRendererFeatureTargetFixture::CreateContactCompute(65, 33);
				(*Results)[8] = ComputeTargets.has_value()
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
					Tests::FRendererFeatureTargetFixture::CreateContactCompute(65, 33);
				(*Results)[9] = RecreatedCompute.has_value()
					&& RecreatedCompute->Visibility.GetReference() != ComputeTexture;
				FRHITexture* RecreatedComputeTexture = RecreatedCompute.has_value()
					? RecreatedCompute->Visibility.GetReference() : nullptr;
				ContactVisibility.ReleaseResources_RenderThread();
				auto ReleasedCompute =
					Tests::FRendererFeatureTargetFixture::CreateContactCompute(65, 33);
				(*Results)[10] = ReleasedCompute.has_value()
					&& ReleasedCompute->Visibility.GetReference()
						!= RecreatedComputeTexture;
				(*Results)[11] = FContactShadowVisibilityRenderer::
					CalculateGroupCount(65) == 9;

				FValidationFrame Frame(CommandList);
				auto MakeInput = [&CommandList](const char* Name,
					EPixelFormat Format, ETextureCreateFlags Flags) {
					return GDynamicRHI->RHICreateTexture(CommandList,
						FRHITextureCreateDesc::Create2D(Name, 65, 33, Format)
							.SetFlags(Flags));
				};
				const ETextureCreateFlags Sampled = ETextureCreateFlags::ShaderResource;
				Material = MakeInput(
					"ContactFailureMaterial", EPixelFormat::RGBA8_UNORM, Sampled);
				Normals = MakeInput(
					"ContactFailureNormals", EPixelFormat::RGBA8_UNORM, Sampled);
				Surface = MakeInput(
					"ContactFailureSurface", EPixelFormat::RGBA8_UNORM, Sampled);
				Emissive = MakeInput(
					"ContactFailureEmissive", EPixelFormat::R11G11B10_FLOAT, Sampled);
				Depth = MakeInput("ContactFailureDepth",
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
				FragmentTargets =
					Tests::FRendererFeatureTargetFixture::CreateContactFragment(65, 33);
				DispatchTargets =
					Tests::FRendererFeatureTargetFixture::CreateContactCompute(65, 33);
			});
		FlushRenderingCommands();
		bool bFirstFrame = true;
		const bool bPrepared = PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
			if (bFirstFrame) Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::ManualRetry, {});
			bFirstFrame = false;
			return ContactVisibility.PrepareRoute_RenderThread(Commands, true, true,
				FragmentTargets.has_value(), DispatchTargets.has_value(), View,
				FVector3(0.0, 0.0, -1.0), 65, 33).Route == FContactShadowVisibilityRenderer::ERoute::Compute;
		});
		PumpValidationFrames([&](FRHICommandListImmediate& CommandList) {
			const auto ComputeOutput = ContactVisibility.Render_RenderThread(
				CommandList, true, &*FragmentTargets, &*DispatchTargets,
				Material, Normals, Surface, Emissive, Depth, View,
				FVector3(0.0, 0.0, -1.0), 65, 33, {});
			(*Results)[12] = bPrepared && ComputeOutput.Route
				== FContactShadowVisibilityRenderer::ERoute::Compute;
			(*Results)[13] = ComputeOutput.Visibility
				== DispatchTargets->Visibility.GetReference();

			ContactVisibility.ReleaseResources_RenderThread();
			FullscreenGeometry.ReleaseResources_RenderThread();
			Material = Normals = Surface = Emissive = Depth = nullptr;
			FragmentTargets.reset();
			DispatchTargets.reset();
			return true;
		});
		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}

	TEST_F(FEditorGridVulkanTests, DeferredDirectionalResourcesRecreateAcrossGenerationChanges)
	{
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FFullscreenGeometryResources FullscreenGeometry;
		FDeferredDirectionalLightingRenderer Deferred(
			Coordinator, FullscreenGeometry
		);
		auto Results = std::make_shared<std::array<bool, 12>>();
		auto FirstTarget = std::make_shared<
			FDeferredDirectionalLightingRenderer::FTargets>();
		auto AlternateTarget = std::make_shared<
			FDeferredDirectionalLightingRenderer::FTargets>();
		std::optional<FDeferredDirectionalLightingRenderer::FTargets> DeviceTarget;
		std::optional<FDeferredDirectionalLightingRenderer::FTargets> ReleasedTarget;
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			(*Results)[0] = Tests::FRendererFeatureTargetFixture::CreateDeferred(64, 32).has_value();
			Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::ManualRetry, {});
			auto Recovered = Tests::FRendererFeatureTargetFixture::CreateDeferred(64, 32);
			(*Results)[1] = Recovered.has_value();
			if (Recovered) *FirstTarget = *Recovered;
			auto Alternate = Tests::FRendererFeatureTargetFixture::CreateDeferred(32, 16);
			(*Results)[2] = Alternate.has_value();
			if (Alternate) *AlternateTarget = *Alternate;
			return true;
		});
		auto InvalidateAndPrepare = [&](ERendererResourceInvalidationCause Cause, size_t Index) {
			bool FirstFrame = true;
			(*Results)[Index] = PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
				if (FirstFrame)
				{
					Coordinator.Apply_RenderThread(Cause, {});
					FirstFrame = false;
				}
				return Deferred.EnsureResources_RenderThread(Commands);
			});
		};
		InvalidateAndPrepare(ERendererResourceInvalidationCause::ManualRetry, 3);
		InvalidateAndPrepare(ERendererResourceInvalidationCause::ManualRetry, 4);
		InvalidateAndPrepare(ERendererResourceInvalidationCause::ShaderChanged, 5);
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			FRHITexture* BeforeDevice = FirstTarget->Color.GetReference();
			Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::Device, {});
			DeviceTarget = Tests::FRendererFeatureTargetFixture::CreateDeferred(64, 32);
			(*Results)[6] = DeviceTarget && DeviceTarget->Color.GetReference() != BeforeDevice;
			return true;
		});
		(*Results)[7] = PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
			return Deferred.EnsureResources_RenderThread(Commands);
		});
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			FRHITexture* BeforeRelease = DeviceTarget ? DeviceTarget->Color.GetReference() : nullptr;
			Deferred.ReleaseResources_RenderThread();
			FullscreenGeometry.ReleaseResources_RenderThread();
			ReleasedTarget = Tests::FRendererFeatureTargetFixture::CreateDeferred(64, 32);
			(*Results)[8] = ReleasedTarget && ReleasedTarget->Color.GetReference() != BeforeRelease;
			(*Results)[10] = ReleasedTarget && ReleasedTarget->Color->GetFormat() == EPixelFormat::RGBA16_FLOAT;
			(*Results)[11] = AlternateTarget->Color != FirstTarget->Color;
			return true;
		});
		(*Results)[9] = PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
			return Deferred.EnsureResources_RenderThread(Commands);
		});
		PumpValidationFrames([&](FRHICommandListImmediate&) {
			DeviceTarget.reset();
			ReleasedTarget.reset();
			return true;
		});

		for (size_t Index = 0; Index < Results->size(); ++Index)
			EXPECT_TRUE((*Results)[Index]) << Index;
		ASSERT_NE(FirstTarget->Color, nullptr);
		EXPECT_EQ(FirstTarget->Color->GetSizeX(), 64u);
		EXPECT_EQ(FirstTarget->Color->GetSizeY(), 32u);
		ASSERT_NE(AlternateTarget->Color, nullptr);
		EXPECT_EQ(AlternateTarget->Color->GetSizeX(), 32u);
		EXPECT_EQ(AlternateTarget->Color->GetSizeY(), 16u);

		EnqueueRenderCommand<FCaptureHDRDisplayContract>(
			[&Deferred, &FullscreenGeometry,
				FirstTarget, AlternateTarget](
				FRHICommandListImmediate&
			) {
				Deferred.ReleaseResources_RenderThread();
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

	TEST_F(FEditorGridVulkanTests, WindowBackedPresentPreservesDisplaySettingsAcrossResizeAndToggles)
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
		RHIInit(FRHIInitializationContext::Presentation({
			.PlatformTarget = Window->GetPresentationTarget()}));
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("HDRDisplayMappingPresentTest");
		RendererLifecycle.Start(Renderer);
		auto Scene = Renderer.CreateScene();
		FSceneViewStateOwner ViewStateOwner = Renderer.CreateViewState();
		ASSERT_TRUE(ViewStateOwner);
		const FSceneViewStateId ViewStateId = ViewStateOwner.GetId();

		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport({
			.PresentationTarget = {.PlatformTarget = Window->GetPresentationTarget()},
			.SizeX = 96,
			.SizeY = 64,
			.PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM,
			.PresentationPolicy = EViewportPresentationPolicy::FramePaced});
		ASSERT_NE(Viewport, nullptr);
		auto RenderPresent = [&Renderer, &Viewport, &Scene, ViewStateId](
								 uint32 Width,
								 uint32 Height,
								 float ExposureEV,
								 bool bEnableFXAA,
								 bool bEnableContactShadows,
								 bool bEditorAssistance,
								 bool bGBufferDebug = false
							 ) {
			const auto Preparation = FRenderResourcePreparation::Create();
			FTextureRHIRef PreparationTarget;
			FSceneView View = bEditorAssistance ? MakeGridView({1.0, 1.0, -0.5}) : FSceneView{};
			View.ViewStateId = ViewStateId;
			View.ViewportWidth = Width;
			View.ViewportHeight = Height;
			View.ClearColor = {4.0f, 2.0f, 0.5f, 0.5f};
			View.Settings.PostProcess.ExposureEV = ExposureEV;
			View.Settings.PostProcess.bEnableFXAA = bEnableFXAA;
			View.Settings.DirectionalShadow.bEnableContactShadows = bEnableContactShadows;
			FSceneViewRenderOptions Options;
			Options.GBufferDebugMode = bGBufferDebug ? EGBufferDebugMode::ReconstructionError : EGBufferDebugMode::Disabled;
			auto Result = ERenderViewResult::RendererResourcesPending;
			PumpValidationFrames([&](FRHICommandListImmediate& Commands) {
				if (!PreparationTarget)
					PreparationTarget = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("PresentPreparation", Width, Height, Viewport->GetFormat())
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
				FScopedRendererQualificationPolicy Qualification({.bEnableDeferredDirectional = bGBufferDebug});
				const auto State = PrepareValidationView(Renderer, Commands,
					{Scene.get(), View, PreparationTarget, true, Options}, Preparation);
				if (State == ERenderResourcePreparationState::Pending) return false;
				if (State != ERenderResourcePreparationState::Ready)
				{
					Result = ERenderViewResult::RendererResourcesUnavailable;
					return true;
				}
				// Acquiring a swapchain image belongs only to the activated draw frame.
				Commands.BeginDrawingViewport(Viewport, nullptr);
				FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
				EXPECT_NE(BackBuffer, nullptr);
				Result = BackBuffer ? Renderer.RenderView(Commands,
					{Scene.get(), View, BackBuffer, true, Options}) : ERenderViewResult::InvalidOutput;
				Commands.EndDrawingViewport(Viewport, Result == ERenderViewResult::Success, false);
				return true;
			});
			PreparationTarget = nullptr;
			return Result;
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
		FSceneInterfaceTestAccess::ReleaseScene(Scene);
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
