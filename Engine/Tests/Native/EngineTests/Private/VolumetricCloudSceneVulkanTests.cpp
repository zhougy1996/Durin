#include <gtest/gtest.h>
#include "VulkanEngineTestSupport.h"

#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "Actors/VolumetricCloudActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Components/VolumetricCloudComponent.h"
#include "CoreGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "SceneViewProjection.h"
#include "SceneTestAccess.h"
#include "Texture/VolumeTexture.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

#include <array>
#include <memory>
#include <vector>

namespace Durin
{
	namespace
	{
		class FSceneCloudTestEngine final : public DEngine
		{
		public:
			FSceneCloudTestEngine()
				: DEngine(FObjectInitializer::Get())
			{
			}
			auto InstallScene(FScenePtr Scene) -> FScene*
			{
				MainScene = std::move(Scene);
				return static_cast<FScene*>(MainScene.get());
			}
			auto ResetScene() -> void { FSceneInterfaceTestAccess::ReleaseScene(MainScene); }
		};

		FViewRenderTelemetry GSceneCloudTelemetry;
		std::vector<FRDGCapture> GSceneCloudGraphCaptures;

		auto CaptureSceneCloudTelemetry(const FViewRenderTelemetry& Telemetry) -> void
		{
			GSceneCloudTelemetry = Telemetry;
		}

		auto CaptureSceneCloudGraph(const FRDGCapture& Capture) -> void
		{
			GSceneCloudGraphCaptures.push_back(Capture);
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

		auto MakeSceneCloudView(uint32 Width, uint32 Height) -> FSceneView
		{
			FSceneView View;
			View.ViewLocation = {0.0, 0.0, 2500.0};
			const FVector3 Forward =
				Math::Normalize(FVector3(1.0, 0.0, 0.1));
			View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
			EXPECT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
				60.0, static_cast<double>(Width) / Height, 0.1, 500000.0,
				ESceneDepthConvention::ReversedZ, View.ProjectionMatrix
			));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Width;
			View.ViewportHeight = Height;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.05f, 0.08f, 0.12f, 1.0f};
			return View;
		}

		auto MakeVolumeAsset(std::string_view Name, uint8 Density)
			-> DVolumeTexture*
		{
			auto* Texture = NewObject<DVolumeTexture>(nullptr, Name);
			FVolumeTextureSourceData Source{
				.Width = 1, .Height = 1, .Depth = 1, .Format = EVolumeTextureFormat::R8_UNORM
			};
			const std::array Voxels{static_cast<std::byte>(Density)};
			EXPECT_TRUE(Source.SetVoxelBytes(Voxels));
			auto Platform = std::make_unique<FVolumeTexturePlatformData>();
			Platform->PixelFormat = EPixelFormat::R8_UNORM;
			Platform->Mips.push_back({.Voxels = {static_cast<std::byte>(Density)},
				.Width = 1, .Height = 1, .Depth = 1, .RowPitch = 1, .DepthPitch = 1});
			std::string Error;
			EXPECT_TRUE(Texture->ApplyBuildResult(
				std::move(Source), {}, std::move(Platform), std::format("scene-cloud-{}", Name), {}, Error))
				<< Error;
			return Texture;
		}

		struct FSceneCloudRender
		{
			static constexpr auto GetName() -> const char*
			{
				return "SceneCloudRender";
			}
		};
	} // namespace

	TEST(FVolumetricCloudSceneVulkanTests, EnabledCloudTraversesOffscreenAndPresentRoutes)
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

		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		InitializeDObjectSystem();
		InitializeApplicationCore();
		auto Window = MakePlatformWindow();
		auto Definition = std::make_shared<FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 0.0f;
		Definition->YDesiredPositionOnScreen = 0.0f;
		Definition->WidthDesiredOnScreen = 96.0f;
		Definition->HeightDesiredOnScreen = 64.0f;
		Definition->Title = "Volumetric cloud scene qualification";
		Window->Initialize(Definition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(Window);

		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(FRHIInitializationContext::Presentation({
			.NativeWindowHandle = Window->GetOSNativeWindowHandle()}));
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("VolumetricCloudSceneVulkan");
		RendererLifecycle.Start(Renderer);
		FSceneCloudTestEngine Engine;
		FScene* Scene = Engine.InstallScene(Renderer.CreateScene());
		GEngine = &Engine;
		auto* World = NewObject<DWorld>(&Engine, "VolumetricCloudSceneWorld");
		ASSERT_TRUE(World->SetCurrentLevel(
			NewObject<DLevel>(World, "VolumetricCloudSceneLevel")
		));
		Engine.SetWorld(World);
		ASSERT_NE(World->SpawnActor<ADirectionalLightActor>("SceneSun"), nullptr);
		auto* Actor = World->SpawnActor<AVolumetricCloudActor>("SceneCloud");
		DVolumetricCloudComponent* Component =
			Actor->GetVolumetricCloudComponent();
		DVolumeTexture* BaseAsset = MakeVolumeAsset("SceneCloudBase", 220);
		DVolumeTexture* DetailAsset = MakeVolumeAsset("SceneCloudDetail", 0);
		Component->SetBaseDensityTexture(BaseAsset);
		Component->SetDetailDensityTexture(DetailAsset);
		Component->RegisterComponent();
		FlushRenderingCommands();
		ASSERT_NE(BaseAsset->GetTextureReferenceRHI(), nullptr);
		ASSERT_NE(DetailAsset->GetTextureReferenceRHI(), nullptr);

		GSceneCloudGraphCaptures.clear();
		SetViewRenderTelemetrySink(CaptureSceneCloudTelemetry);
		SetSceneRenderGraphCaptureSink(CaptureSceneCloudGraph);
		auto RenderOffscreen = [&Renderer, Scene](
								   bool bForceFragment
							   ) {
			auto Pixels = std::make_shared<Durin::FByteArray>();
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Scene, Pixels, Result, bForceFragment](
					FRHICommandListImmediate& CommandList
				) {
					constexpr uint32 Width = 96;
					constexpr uint32 Height = 64;
					FTextureRHIRef Output = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
										 "SceneCloudOffscreen", Width, Height,
										 EPixelFormat::SRGBA8_UNORM
									 )
										 .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback)
					);
					if (!Output) return;
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FSceneView View = MakeSceneCloudView(Width, Height);
					FScopedRendererQualificationPolicy Qualification({
						.bForceFragmentVolumetricCloud = bForceFragment});
					*Result = Renderer.RenderView(
						CommandList, Scene, View, Output, false, {}
					);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, *Pixels
					);
				}
			);
			FlushRenderingCommands();
			EXPECT_EQ(*Result, ERenderViewResult::Success);
			return Pixels;
		};

		Component->SetEnabled(false);
		const auto Disabled = RenderOffscreen(false);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudDisabledViews, 1u);
		Component->SetEnabled(true);
		Component->SetBaseDensityTexture(nullptr);
		const auto InvalidRequiredInput = RenderOffscreen(false);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudDisabledViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudEnabledViews, 0u);
		EXPECT_EQ(*InvalidRequiredInput, *Disabled);
		Component->SetBaseDensityTexture(BaseAsset);
		const auto Compute = RenderOffscreen(false);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudComputeViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudDispatches, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudCompositeDraws, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowComputeViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowDispatches, 1u);
		EXPECT_GT(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowSamples, 0u);
		const auto Fragment = RenderOffscreen(true);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudFragmentViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudDraws, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudCompositeDraws, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowFragmentViews, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudShadowDraws, 1u);
		ASSERT_EQ(Compute->size(), Fragment->size());
		ASSERT_EQ(Disabled->size(), Compute->size());
		bool bDiffersFromDisabled = false;
		for (size_t Index = 0; Index < Compute->size(); ++Index)
		{
			EXPECT_LE(std::abs(static_cast<int>((*Compute)[Index]) - static_cast<int>((*Fragment)[Index])), 1) << Index;
			bDiffersFromDisabled |= (*Compute)[Index] != (*Disabled)[Index];
		}
		EXPECT_TRUE(bDiffersFromDisabled);

		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport({.NativeWindowHandle = Window->GetOSNativeWindowHandle(), .SizeX = 96, .SizeY = 64, .PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM, .PresentationPolicy = EViewportPresentationPolicy::FramePaced});
		ASSERT_NE(Viewport, nullptr);
		auto RenderPresent = [&Renderer, &Viewport, Scene](uint32 Width, uint32 Height, bool bForceFragment) {
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Scene, Viewport, Width, Height, Result, bForceFragment](
					FRHICommandListImmediate& CommandList
				) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					CommandList.BeginDrawingViewport(Viewport, nullptr);
					FTextureRHIRef BackBuffer =
						GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
					if (BackBuffer)
					{
						FSceneView View = MakeSceneCloudView(Width, Height);
						FScopedRendererQualificationPolicy Qualification({
							.bForceFragmentVolumetricCloud = bForceFragment});
						*Result = Renderer.RenderView(
							CommandList, Scene, View, BackBuffer, true, {}
						);
					}
					CommandList.EndDrawingViewport(Viewport, true, false);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			);
			FlushRenderingCommands();
			return *Result;
		};

		EXPECT_EQ(RenderPresent(96, 64, false), ERenderViewResult::Success);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudComputeViews, 1u);
		GDynamicRHI->RHIResizeViewport(Viewport, 128, 72, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(128, 72, true), ERenderViewResult::Success);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudFragmentViews, 1u);
		ASSERT_EQ(GSceneCloudGraphCaptures.size(), 6u);
		// Post process now declares its typed isolated-deferred result read; the
		// former side-channel lookup carried no graph edge.
		const std::array<uint32, 6> ExpectedDependencies{23, 23, 26, 26, 26, 26};
		const std::array<uint32, 6> ExpectedTextureTransitions{1, 1, 17, 1, 17, 1};
		for (size_t Index = 0; Index < GSceneCloudGraphCaptures.size(); ++Index)
		{
			const auto& Statistics = GSceneCloudGraphCaptures[Index].Statistics;
			EXPECT_EQ(Statistics.DeclaredPasses, 11u) << Index;
			EXPECT_EQ(Statistics.ScheduledPasses, 11u) << Index;
			EXPECT_EQ(Statistics.Dependencies, ExpectedDependencies[Index]) << Index;
			EXPECT_EQ(Statistics.BufferTransitions, 0u) << Index;
			EXPECT_EQ(Statistics.TextureTransitions,
				ExpectedTextureTransitions[Index]) << Index;
			EXPECT_FALSE(Statistics.bCompileBudgetExceeded) << Index;
			EXPECT_FALSE(Statistics.bExecuteBudgetExceeded) << Index;
			const auto& Allocation =
				GSceneCloudGraphCaptures[Index].AllocationStatistics;
			EXPECT_GT(Allocation.ActiveResources, 0u) << Index;
			EXPECT_GE(Allocation.RetainedResources,
				Allocation.ActiveResources) << Index;
			EXPECT_GE(Allocation.RetainedBytes, Allocation.ActiveBytes) << Index;
			EXPECT_EQ(Allocation.Failures, 0u) << Index;
		}
		EXPECT_GT(GSceneCloudGraphCaptures.back().AllocationStatistics.ReuseHits,
			0u);
		EXPECT_GT(GSceneCloudGraphCaptures.back().AllocationStatistics.ReuseMisses,
			0u);
		const auto& FinalAllocation =
			GSceneCloudGraphCaptures.back().AllocationStatistics;
		std::cout << "RDG_SCENE_ALLOCATION active_resources="
			<< FinalAllocation.ActiveResources << ",retained_resources="
			<< FinalAllocation.RetainedResources << ",active_bytes="
			<< FinalAllocation.ActiveBytes << ",retained_bytes="
			<< FinalAllocation.RetainedBytes << ",reuse_hits="
			<< FinalAllocation.ReuseHits << ",reuse_misses="
			<< FinalAllocation.ReuseMisses << ",evictions="
			<< FinalAllocation.Evictions << '\n';

		SetSceneRenderGraphCaptureSink(nullptr);
		SetViewRenderTelemetrySink(nullptr);
		Viewport = nullptr;
		Component->UnregisterComponent();
		Engine.SetWorld(nullptr);
		Engine.ResetScene();
		GEngine = nullptr;
		MarkObjectHierarchyAsGarbage(World);
		MarkAsGarbage(BaseAsset);
		MarkAsGarbage(DetailAsset);
		CollectGarbage();
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
