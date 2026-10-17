#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "Asset/AssetCompilingManager.h"
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
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "Math/Operations.h"
#include "NativeTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "SceneViewProjection.h"
#include "SceneTestAccess.h"
#include "Texture/VolumeTexture.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

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
		uint32 GSceneCloudTelemetryPublications = 0;
		std::vector<FRDGCapture> GSceneCloudGraphCaptures;
		std::filesystem::path GSceneCloudEvidenceRoot;

		auto CaptureSceneCloudTelemetry(const FViewRenderTelemetry& Telemetry) -> void
		{
			GSceneCloudTelemetry = Telemetry;
			++GSceneCloudTelemetryPublications;
		}

		auto CaptureSceneCloudGraph(const FRDGCapture& Capture) -> void
		{
			GSceneCloudGraphCaptures.push_back(Capture);
			std::ofstream Output(GSceneCloudEvidenceRoot /
				("graph-" + std::to_string(GSceneCloudGraphCaptures.size()) + ".txt"));
			Output << Capture.Dump;
			EXPECT_TRUE(Output.good());
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
			Platform->Mips.push_back({.Voxels = FSharedByteBuffer::Take(FByteBuffer{static_cast<std::byte>(Density)}),
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

	inline auto QualifySceneRenderRoutes(bool bPresentation) -> void
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
		std::shared_ptr<FGenericWindow> Window;
		if (bPresentation)
		{
			InitializeApplicationCore();
			Window = MakePlatformWindow();
			auto Definition = std::make_shared<FGenericWindowDefinition>();
			Definition->XDesiredPositionOnScreen = 0.0f;
			Definition->YDesiredPositionOnScreen = 0.0f;
			Definition->WidthDesiredOnScreen = 96.0f;
			Definition->HeightDesiredOnScreen = 64.0f;
			Definition->Title = "Volumetric cloud scene qualification";
			Window->Initialize(Definition);
			ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
			GApp = std::make_shared<FTestApplication>(Window);
		}

		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		RHIInit(bPresentation ? FRHIInitializationContext::Presentation({
			.PlatformTarget = Window->GetPresentationTarget()}) : FRHIInitializationContext::Headless());
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
		GSceneCloudTelemetryPublications = 0;
		const char* Mode = std::getenv("DURIN_RHI_EXECUTION");
		const char* Backend = std::getenv("DURIN_RHI_BACKEND");
		GSceneCloudEvidenceRoot = Testing::CreateTestFixtureDirectory(
			std::string("SceneRenderRoutes-") + (Backend ? Backend : "default") + "-" + (Mode ? Mode : "default"));
		SetViewRenderTelemetrySink(CaptureSceneCloudTelemetry);
		SetSceneRenderGraphCaptureSink(CaptureSceneCloudGraph);
		FRendererQualificationPolicy ExtraPolicy;
		FSceneViewRenderOptions ExtraOptions;
		bool bContactShadows = false;
		uint32 OffscreenOrdinal = 0;
		auto RenderOffscreen = [&Renderer, Scene, &ExtraPolicy, &ExtraOptions, &bContactShadows, &OffscreenOrdinal](
								   bool bForceFragment, bool bAmbientOcclusion = true,
			EGroundTruthAmbientOcclusionQuality AOQuality = EGroundTruthAmbientOcclusionQuality::HalfResolution,
			ERenderMode RenderMode = ERenderMode::Lit,
			ESceneRasterFailure Failure = ESceneRasterFailure::None,
			uint32 Width = 96, uint32 Height = 64
							   ) {
			const uint32 PublicationsBefore = GSceneCloudTelemetryPublications;
			auto Pixels = std::make_shared<Durin::FByteBuffer>();
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Scene, Pixels, Result, bForceFragment, bAmbientOcclusion, AOQuality, RenderMode,
					Failure, Width, Height, Policy = ExtraPolicy, Options = ExtraOptions, bContactShadows](
					FRHICommandListImmediate& CommandList
				) {
					FTextureRHIRef Output = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
										 "SceneCloudOffscreen", Width, Height,
										 EPixelFormat::SRGBA8_UNORM
									 )
										 .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback)
					);
					if (!Output) return;
					CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FSceneView View = MakeSceneCloudView(Width, Height);
					View.Settings.Mode.RenderMode = RenderMode;
					View.Settings.AmbientOcclusion.bEnabled = bAmbientOcclusion;
					View.Settings.AmbientOcclusion.Quality = AOQuality;
					View.Settings.DirectionalShadow.bEnableContactShadows = bContactShadows;
					auto SelectedPolicy = Policy;
					SelectedPolicy.bForceFragmentVolumetricCloud = bForceFragment;
					SelectedPolicy.RasterFailure = Failure;
					FScopedRendererQualificationPolicy Qualification(SelectedPolicy);
					*Result = Renderer.RenderView(
						CommandList, Scene, View, Output, false, Options
					);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					if (*Result == ERenderViewResult::Success)
					{
						GDynamicRHI->RHIReadTexture2D(CommandList, Output, 0, 0, *Pixels);
					}
				}
			);
			FlushRenderingCommands();
			const auto Expected = Failure == ESceneRasterFailure::HybridBootstrap || Failure == ESceneRasterFailure::Forward
				? ERenderViewResult::RequiredEnvironmentUnavailable
				: Failure != ESceneRasterFailure::None ? ERenderViewResult::RendererResourcesUnavailable : ERenderViewResult::Success;
			EXPECT_EQ(*Result, Expected);
			++OffscreenOrdinal;
			std::cout << "RDG_SCENE_CASE index=" << OffscreenOrdinal
				<< ",mode=" << static_cast<uint32>(RenderMode) << ",ao=" << bAmbientOcclusion
				<< ",ao_quality=" << static_cast<uint32>(AOQuality) << ",fragment=" << bForceFragment
				<< ",diagnostic_gbuffer=" << ExtraPolicy.bEnableGBuffer
				<< ",gbuffer_debug=" << static_cast<uint32>(ExtraOptions.GBufferDebugMode)
				<< ",isolated_deferred=" << ExtraPolicy.bEnableDeferredDirectional
				<< ",contact=" << bContactShadows << ",failure=" << static_cast<uint32>(Failure)
				<< ",width=" << Width << ",height=" << Height << ",bytes=" << Pixels->size() << '\n';
			if (Expected == ERenderViewResult::Success)
			{
				EXPECT_EQ(GSceneCloudTelemetryPublications, PublicationsBefore + 1);
				EXPECT_EQ(Pixels->size(), static_cast<size_t>(Width) * Height * 4);
				std::ofstream Output(GSceneCloudEvidenceRoot /
					("pixels-" + std::to_string(OffscreenOrdinal) + ".rgba"), std::ios::binary);
				Output.write(reinterpret_cast<const char*>(Pixels->data()), Pixels->size());
				EXPECT_TRUE(Output.good());
			}
			else
			{
				EXPECT_TRUE(Pixels->empty());
				EXPECT_EQ(GSceneCloudTelemetryPublications, PublicationsBefore);
			}
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

		TRefCountPtr<FRHIViewport> Viewport;
		if (bPresentation)
		{
			Viewport = GDynamicRHI->RHICreateViewport({.PresentationTarget = {.PlatformTarget = Window->GetPresentationTarget()}, .SizeX = 96, .SizeY = 64, .PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM, .PresentationPolicy = EViewportPresentationPolicy::FramePaced});
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
		}
		else
		{
			RenderOffscreen(false);
			RenderOffscreen(true, true, EGroundTruthAmbientOcclusionQuality::HalfResolution,
				ERenderMode::Lit, ESceneRasterFailure::None, 128, 72);
		}
		ASSERT_EQ(GSceneCloudGraphCaptures.size(), 6u);
		// Contact shadows are disabled throughout; absent cloud inputs also omit
		// the cloud-shadow producer and its completion dependencies.
		// Shadow recording declares three layer passes and one typed consumer.
		// Three visible Base Scene stages replace the callback plus its old
		// production-binding preparation pass: one net pass and one net edge.
		const std::array<uint32, 6> ExpectedPasses{11, 11, 14, 14, 14, 14};
		const std::array<uint32, 6> ExpectedDependencies{20, 20, 30, 30, 30, 30};
		// RDG also emits entry handoffs for discarded render-pass attachments and
		// same-state writes; render-pass-owned final transitions do not replace them.
		// Logical handoffs preserve each of the three directional-shadow layers.
		// Former native-pass transitions now appear as graph barriers. Fragment
		// cloud visibility also needs two explicit depth handoffs around raster work.
		const std::array<uint32, 6> ExpectedTextureSubresources{22, 22, 39, 27, 39, 27};
		// Each independent shadow list now has its own entry barrier.
		const std::array<uint32, 6> ExpectedTextureTransitions{22, 22, 39, 27, 39, 27};
		for (size_t Index = 0; Index < GSceneCloudGraphCaptures.size(); ++Index)
		{
			const auto& Statistics = GSceneCloudGraphCaptures[Index].Statistics;
			const auto& Capture = GSceneCloudGraphCaptures[Index];
			EXPECT_FALSE(std::ranges::any_of(Capture.Resources, [](const auto& Resource) {
				return Resource.Name == "Scene.ProductionDeferredParameters";
			}));
			EXPECT_FALSE(std::ranges::any_of(Capture.Passes, [](const auto& Pass) {
				return Pass.Name == "Scene.DeferredDirectionalLighting";
			}));
			for (const auto Name : {"Scene.HybridBootstrap", "Scene.ProductionDeferred", "Scene.RetainedForward", "Scene.Color"})
			{
				const auto Pass = std::ranges::find(Capture.Passes, Name, &FRDGPassCapture::Name);
				ASSERT_NE(Pass, Capture.Passes.end()) << Name;
				for (const auto& Parameter : Capture.Parameters)
					if (Parameter.PassDeclarationIndex == Pass->DeclarationIndex)
						EXPECT_FALSE(Parameter.bPassManagedTransition) << Name << ':' << Parameter.FieldPath;
			}
			const auto Lighting = std::ranges::find(Capture.Passes, "Scene.ProductionDeferred", &FRDGPassCapture::Name);
			for (const auto Name : {"Scene.GBuffer.Material", "Scene.GBuffer.Normals", "Scene.GBuffer.Surface", "Scene.GBuffer.Emissive", "Scene.Depth"})
			{
				const auto Resource = std::ranges::find(Capture.Resources, Name, &FRDGResourceCapture::Name);
				ASSERT_NE(Resource, Capture.Resources.end());
				EXPECT_TRUE(std::ranges::any_of(Capture.Uses, [&](const auto& Use) {
					return Use.PassDeclarationIndex == Lighting->DeclarationIndex && Use.ResourceId == Resource->ResourceId
						&& Use.Use == ERDGUse::Read && Use.Access == ERHIAccess::GraphicsShaderRead;
				})) << Name;
			}
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
			EXPECT_EQ(Statistics.TextureTransitionSubresources, ExpectedTextureSubresources[Index]) << Index;
			EXPECT_EQ(Statistics.TextureTransitions,
				ExpectedTextureTransitions[Index]) << Index << '\n' << GSceneCloudGraphCaptures[Index].Dump;
			EXPECT_LE(Capture.ExecutionPlan.Batches.size(), 4u) << Index;
			// Wall-clock RDG timings vary with host contention and cold driver work.
			// Keep them diagnostic; performance gates belong in qualification tests.
			std::cout << "RDG_SCENE_TIMING capture=" << Index
				<< ",passes=" << Statistics.ScheduledPasses
				<< ",dependencies=" << Statistics.Dependencies
				<< ",texture_transitions=" << Statistics.TextureTransitions
				<< ",submission_batches=" << Capture.ExecutionPlan.Batches.size()
				<< ",record_us=" << Statistics.Phases.RecordingMicroseconds
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
		EXPECT_EQ(WithoutAO.Statistics.DeclaredPasses, 13u);
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

		// Give the extended routes real opaque and retained geometry, rather than
		// mistaking an empty-scene fallback for native shading coverage.
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
			ASSERT_TRUE(InitializeAssetCompilingManager());
		auto MakeRouteMaterial = [](const char* Name, EMaterialShadingModel Shading,
			EMaterialBlendMode Blend = EMaterialBlendMode::Opaque) {
			auto* Material = NewObject<DMaterial>(nullptr, Name);
			AddToRoot(Material);
			FMaterialStaticProperties Properties;
			Properties.ShadingModel = Shading;
			Properties.BlendMode = Blend;
			Properties.bTwoSided = true;
			EXPECT_TRUE(Material->SetStaticProperties(Properties));
			auto* Color = NewObject<DMaterialExpressionVector3Constant>(Material, "Color");
			Color->Id = {1, 2, 3, 4};
			Color->Value = Blend == EMaterialBlendMode::Translucent ? FVector3{0.1, 0.6, 0.9} : FVector3{0.7, 0.25, 0.1};
			std::vector<DMaterialExpression*> Expressions{Color};
			FMaterialExpressionSurfaceOutputs Outputs;
			Outputs.BaseColor = {Color->Id};
			if (Blend != EMaterialBlendMode::Opaque)
			{
				auto* Opacity = NewObject<DMaterialExpressionScalarConstant>(Material, "Opacity");
				Opacity->Id = {1, 2, 3, 5};
				Opacity->Value = Blend == EMaterialBlendMode::Translucent ? 0.5f : 1.0f;
				Expressions.push_back(Opacity);
				if (Blend == EMaterialBlendMode::Translucent) Outputs.Opacity = {Opacity->Id};
				else Outputs.OpacityMask = {Opacity->Id};
			}
			EXPECT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
			EXPECT_TRUE(Material->CompileEdits());
			DObject* Object = Material;
			FAssetCompilingManager::Get().FinishCompilationForObjects(std::span<DObject* const>(&Object, 1));
			EXPECT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
			return Material;
		};
		auto* LitMaterial = MakeRouteMaterial("SceneRouteLit", EMaterialShadingModel::Lit);
		auto* UnlitMaterial = MakeRouteMaterial("SceneRouteUnlit", EMaterialShadingModel::Unlit);
		auto* MaskedMaterial = MakeRouteMaterial("SceneRouteMasked", EMaterialShadingModel::Lit, EMaterialBlendMode::Masked);
		auto* TranslucentMaterial = MakeRouteMaterial("SceneRouteTranslucent", EMaterialShadingModel::Unlit, EMaterialBlendMode::Translucent);
		auto Geometry = std::make_unique<FStaticMeshRenderData>();
		Geometry->MaterialSlots = {{"Route", 0}};
		auto& LOD = Geometry->LODResources.emplace_back();
		LOD.VertexBuffers.PositionVertexBuffer.Init({{0, -40, -40}, {0, 40, -40}, {0, 40, 40}, {0, -40, 40}});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(4, {-1, 0, 0}), std::vector<FVector4f>(4, {0, 1, 0, 1}));
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
		UVs[0] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), 4, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(4, FVector4f(1)), 4);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({.Name = "Route", .FirstIndex = 0, .IndexCount = 6,
			.MinVertexIndex = 0, .MaxVertexIndex = 3, .MaterialSlotIndex = 0,
			.LocalBounds = FBox({0, -40, -40}, {0, 40, 40})});
		LOD.LocalBounds = LOD.Sections[0].LocalBounds;
		Geometry->LODVertexFactories.resize(1);
		Geometry->RecalculateBounds();
		EnqueueRenderCommand<FSceneCloudRender>([&](FRHICommandListImmediate& Commands) {
			ASSERT_TRUE(Geometry->InitResources(Commands));
		});
		FlushRenderingCommands();
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10001),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{LitMaterial->GetMaterialRenderProxy()}),
			Math::TranslationMatrix(FVector3{100, -35, 2510}));
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10002),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{UnlitMaterial->GetMaterialRenderProxy()}),
			Math::TranslationMatrix(FVector3{100, 35, 2510}));
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10003),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{MaskedMaterial->GetMaterialRenderProxy()}),
			Math::TranslationMatrix(FVector3{95, -35, 2520}));
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10004),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{TranslucentMaterial->GetMaterialRenderProxy()}),
			Math::TranslationMatrix(FVector3{90, 35, 2510}));
		FlushRenderingCommands();

		const auto HasPass = [](const char* Name) {
			return std::ranges::any_of(GSceneCloudGraphCaptures.back().Passes,
				[&](const auto& Pass) { return Pass.Name == Name; });
		};
		ExtraPolicy.bEnableGBuffer = true;
		ExtraOptions.GBufferDebugMode = EGBufferDebugMode::Material;
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit);
		EXPECT_TRUE(HasPass("Scene.Forward"));
		EXPECT_FALSE(HasPass("Scene.ProductionDeferred"));
		EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GBufferEnabledViews, 1u);
		EXPECT_GT(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 0u);
		ExtraOptions = {};
		ExtraPolicy.bEnableDeferredDirectional = true;
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit);
		EXPECT_TRUE(HasPass("Scene.Forward"));
		EXPECT_TRUE(HasPass("Scene.DeferredDirectionalLighting"));
		EXPECT_FALSE(HasPass("Scene.ProductionDeferred"));
		RenderOffscreen(false);
		EXPECT_TRUE(HasPass("Scene.DeferredDirectionalLighting"));
		EXPECT_TRUE(HasPass("Scene.ProductionDeferred"));
		EXPECT_EQ(GSceneCloudTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
		EXPECT_GT(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 0u);
		EXPECT_GT(GSceneCloudTelemetry.StaticMesh.StaticMeshSuccessfulDraws, 0u);
		EXPECT_GE(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 2u);
		EXPECT_GT(GSceneCloudTelemetry.CombinedTranslucentGeometryDraws, 0u);
		ExtraPolicy = {};
		bContactShadows = true;
		const auto ComputeContact = RenderOffscreen(false);
		EXPECT_TRUE(HasPass("Scene.ContactShadowVisibility"));
		EXPECT_EQ(GSceneCloudTelemetry.ContactShadow.ContactShadowDispatches, 1u);
		ExtraPolicy.bForceFragmentContactVisibility = true;
		const auto FragmentContact = RenderOffscreen(false);
		EXPECT_TRUE(HasPass("Scene.ContactShadowVisibility"));
		EXPECT_EQ(GSceneCloudTelemetry.ContactShadow.ContactShadowDraws, 1u);
		EXPECT_EQ(*ComputeContact, *FragmentContact);
		ExtraPolicy = {};
		bContactShadows = false;

		// Each injected raster failure executes the real authored route and must
		// survive later cloud/sorted stages without becoming a successful publication.
		for (const auto Failure : {ESceneRasterFailure::HybridBootstrap, ESceneRasterFailure::ProductionDeferred,
			ESceneRasterFailure::RetainedForward, ESceneRasterFailure::SortedTranslucency})
			RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Lit, Failure);
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit, ESceneRasterFailure::Forward);

		SetSceneRenderGraphCaptureSink(nullptr);
		SetViewRenderTelemetrySink(nullptr);
		Viewport = nullptr;
		Component->UnregisterComponent();
		Engine.SetWorld(nullptr);
		Engine.ResetScene();
		GEngine = nullptr;
		RemoveFromRoot(LitMaterial);
		RemoveFromRoot(UnlitMaterial);
		RemoveFromRoot(MaskedMaterial);
		RemoveFromRoot(TranslucentMaterial);
		MarkObjectHierarchyAsGarbage(LitMaterial);
		MarkObjectHierarchyAsGarbage(UnlitMaterial);
		MarkObjectHierarchyAsGarbage(MaskedMaterial);
		MarkObjectHierarchyAsGarbage(TranslucentMaterial);
		MarkObjectHierarchyAsGarbage(World);
		MarkAsGarbage(BaseAsset);
		MarkAsGarbage(DetailAsset);
		CollectGarbage();
		FlushRenderingCommands();
		EnqueueRenderCommand<FSceneCloudRender>([&](FRHICommandListImmediate&) { Geometry->ReleaseResources(); });
		FlushRenderingCommands();
		RendererLifecycle.Shutdown();
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
		if (bPresentation)
		{
			GApp = nullptr;
			Window.reset();
			ShutdownApplicationCore();
		}
	}
} // namespace Durin
