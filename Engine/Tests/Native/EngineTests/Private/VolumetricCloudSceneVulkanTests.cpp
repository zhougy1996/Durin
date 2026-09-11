#include "Threading/Task.h"
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
			auto PreparedTextureSource = Durin::PrepareVolumeTextureSource(Source);
			EXPECT_TRUE(PreparedTextureSource);
			if (!PreparedTextureSource) return nullptr;
			Texture->SetSource(std::move(*PreparedTextureSource));
			Texture->SetBuildSettings({});
			Texture->SetPlatformData(std::move(Platform));
			Texture->UpdateResource();
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
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
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
		EXPECT_TRUE(World->InitializeSubsystems());
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
								   bool bForceFragment, bool bAmbientOcclusion = true,
			EGroundTruthAmbientOcclusionQuality AOQuality = EGroundTruthAmbientOcclusionQuality::HalfResolution,
			ERenderMode RenderMode = ERenderMode::Lit
							   ) {
			auto Pixels = std::make_shared<Durin::FByteBuffer>();
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Scene, Pixels, Result, bForceFragment, bAmbientOcclusion, AOQuality, RenderMode](
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
					View.Settings.Mode.RenderMode = RenderMode;
					View.Settings.AmbientOcclusion.bEnabled = bAmbientOcclusion;
					View.Settings.AmbientOcclusion.Quality = AOQuality;
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
		// Contact shadows are disabled throughout; absent cloud inputs also omit
		// the cloud-shadow producer and its completion dependencies.
		const std::array<uint32, 6> ExpectedPasses{7, 7, 10, 10, 10, 10};
		const std::array<uint32, 6> ExpectedDependencies{11, 11, 21, 21, 21, 21};
		// RDG also emits entry handoffs for discarded render-pass attachments and
		// same-state writes; render-pass-owned final transitions do not replace them.
		// Logical handoffs preserve each of the three directional-shadow layers.
		const std::array<uint32, 6> ExpectedTextureTransitions{15, 15, 32, 18, 32, 18};
		for (size_t Index = 0; Index < GSceneCloudGraphCaptures.size(); ++Index)
		{
			const auto& Statistics = GSceneCloudGraphCaptures[Index].Statistics;
			const auto& Capture = GSceneCloudGraphCaptures[Index];
			const auto Shadow = std::ranges::find(Capture.Resources, "Scene.DirectionalShadow", &FRDGResourceCapture::Name);
			ASSERT_NE(Shadow, Capture.Resources.end());
			for (uint16 Layer = 0; Layer < 3; ++Layer)
				EXPECT_EQ(std::ranges::count_if(Capture.Transitions, [&](const auto& Transition) {
					return Transition.ResourceId == Shadow->ResourceId
						&& Transition.Kind == ERDGTransitionKind::RHIBarrier
						&& Transition.bDiscardContents && !Transition.bFinal
						&& Transition.TextureRange.FirstArrayLayer == Layer
						&& Transition.TextureRange.NumArrayLayers == 1;
				}), 1) << "shadow layer=" << Layer << " capture=" << Index;
			// Frame-local backing has no external final-state consumer.
			for (const auto& Transition : Capture.Transitions)
				if (Transition.bFinal)
				{
					const auto Resource = std::ranges::find_if(Capture.Resources,
						[&](const auto& Item) { return Item.ResourceId == Transition.ResourceId; });
					ASSERT_NE(Resource, Capture.Resources.end());
					EXPECT_TRUE(Resource->bExternal) << Resource->Name;
				}

			// The feature boundary publishes complete GBuffer and half-resolution AO sets.
			for (const auto Name : {"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
				"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive", "Scene.AmbientOcclusion.Raw",
				"Scene.AmbientOcclusion.Scratch", "Scene.AmbientOcclusion.Selector",
				"Scene.AmbientOcclusion.Resolved"})
				EXPECT_EQ(std::ranges::count_if(Capture.Resources, [&](const auto& Resource) {
					return Resource.Name == Name && Resource.Preparation != "culled";
				}), 1) << Name << " capture=" << Index;
			EXPECT_FALSE(std::ranges::any_of(Capture.Passes, [](const auto& Pass) {
				return Pass.Name == "Scene.ContactShadowVisibility";
			}));
			EXPECT_EQ(std::ranges::any_of(Capture.Passes, [](const auto& Pass) {
				return Pass.Name == "Scene.VolumetricCloudShadow";
			}), Index >= 2);
			EXPECT_FALSE(std::ranges::any_of(Capture.Resources, [](const auto& Resource) {
				return Resource.Name == "Scene.ContactShadowVisibilityValue";
			}));
			EXPECT_EQ(std::ranges::any_of(Capture.Resources, [](const auto& Resource) {
				return Resource.Name == "Scene.CloudShadowValue";
			}), Index >= 2);
			for (const auto Name : {"Scene.VolumetricCloudSpatial", "Scene.VolumetricCloud"})
				EXPECT_EQ(std::ranges::any_of(Capture.Passes, [&](const auto& Pass) {
					return Pass.Name == Name;
				}), Index >= 2) << Name;
			for (const auto Name : {"Scene.VolumetricCloudSpatialValue", "Scene.VolumetricCloudValue"})
				EXPECT_EQ(std::ranges::any_of(Capture.Resources, [&](const auto& Resource) {
					return Resource.Name == Name;
				}), Index >= 2) << Name;
			EXPECT_EQ(Statistics.DeclaredPasses, ExpectedPasses[Index]) << Index;
			EXPECT_EQ(Statistics.ScheduledPasses, ExpectedPasses[Index]) << Index;
			EXPECT_EQ(Statistics.Dependencies, ExpectedDependencies[Index]) << Index;
			EXPECT_EQ(Statistics.BufferTransitions, 0u) << Index;
			EXPECT_EQ(Statistics.TextureTransitions,
				ExpectedTextureTransitions[Index]) << Index << '\n' << GSceneCloudGraphCaptures[Index].Dump;
			// Wall-clock RDG timings vary with host contention and cold driver work.
			// Keep them diagnostic; performance gates belong in qualification tests.
			std::cout << "RDG_SCENE_TIMING capture=" << Index
				<< ",compile_us=" << Statistics.CompileMicroseconds
				<< ",execute_us=" << Statistics.ExecuteMicroseconds
				<< ",compile_budget_exceeded=" << Statistics.bCompileBudgetExceeded
				<< ",execute_budget_exceeded=" << Statistics.bExecuteBudgetExceeded
				<< '\n';
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

		RenderOffscreen(false, false);
		ASSERT_EQ(GSceneCloudGraphCaptures.size(), 7u);
		const auto& WithoutAO = GSceneCloudGraphCaptures.back();
		EXPECT_EQ(WithoutAO.Statistics.DeclaredPasses, 9u);
		EXPECT_FALSE(std::ranges::any_of(WithoutAO.Passes, [](const auto& Pass) {
			return Pass.Name == "Scene.AmbientOcclusion";
		}));
		EXPECT_FALSE(std::ranges::any_of(WithoutAO.Resources, [](const auto& Resource) {
			return Resource.Name.starts_with("Scene.AmbientOcclusion");
		}));
		EXPECT_EQ(GSceneCloudTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews, 0u);

		RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::FullResolution);
		ASSERT_EQ(GSceneCloudGraphCaptures.size(), 8u);
		const auto& FullResolution = GSceneCloudGraphCaptures.back();
		for (const auto Name : {"Scene.AmbientOcclusion.Raw", "Scene.AmbientOcclusion.Scratch"})
		{
			const auto Resource = std::ranges::find_if(FullResolution.Resources,
				[&](const auto& Candidate) { return Candidate.Name == Name; });
			ASSERT_NE(Resource, FullResolution.Resources.end()) << Name;
			EXPECT_EQ(Resource->TextureExtent.x, 96u);
			EXPECT_EQ(Resource->TextureExtent.y, 64u);
		}
		EXPECT_FALSE(std::ranges::any_of(FullResolution.Resources, [](const auto& Resource) {
			return Resource.Name == "Scene.AmbientOcclusion.Selector"
				|| Resource.Name == "Scene.AmbientOcclusion.Resolved";
		}));
		EXPECT_EQ(GSceneCloudTelemetry.AmbientOcclusion.GroundTruthAmbientOcclusionFullResolutionViews, 1u);

		// Forward-only output needs neither GBuffer nor cloud work.
		Component->SetEnabled(false);
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::FullResolution,
			ERenderMode::Unlit);
		ASSERT_EQ(GSceneCloudGraphCaptures.size(), 9u);
		const auto& ForwardOnly = GSceneCloudGraphCaptures.back();
		for (const auto Name : {"Scene.GBuffer", "Scene.VolumetricCloudSpatial", "Scene.VolumetricCloud"})
			EXPECT_FALSE(std::ranges::any_of(ForwardOnly.Passes, [&](const auto& Pass) {
				return Pass.Name == Name;
			})) << Name;
		EXPECT_FALSE(std::ranges::any_of(ForwardOnly.Resources, [](const auto& Resource) {
			return Resource.Name.starts_with("Scene.GBuffer")
				|| Resource.Name.starts_with("Scene.VolumetricCloud");
		}));
		EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GBufferEnabledViews, 0u);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudDisabledViews, 1u);

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
