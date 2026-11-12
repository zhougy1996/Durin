#pragma once

#include "CoreMinimal.h"
#include "Threading/TaskRuntime.h"

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
#include "LightSceneTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialProgramCompiler.h"
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
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RendererResourceCoordinator.h"
#include "SceneViewProjection.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/Shader.h"
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

		std::unique_ptr<FScopedRendererQualificationPolicy> GRecordingQualificationOverride;
		auto OverrideQualificationAfterAuthoring(uint64) -> void
		{
			GRecordingQualificationOverride = std::make_unique<FScopedRendererQualificationPolicy>(
				FRendererQualificationPolicy{.GPUCullingFailure = EGPUCullingFailure::CommandAdmission}
			);
		}

		FViewRenderTelemetry GSceneCloudTelemetry;
		// Read after the offscreen submission and its synchronous readback have retired.
		std::array<FGPUTimingQueryRHIRef, 2> GLocalLightTimingQueries;
		uint64 GLocalLightFrameNanoseconds = 0;
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

		FShaderRHIRef GCullingDepthShader;
		FComputePipelineStateRHIRef GCullingDepthPipeline;
		std::array<std::shared_ptr<FRHITextureReadback>, 5> GCullingAttachments;
		auto CaptureCullingAttachments(FRHICommandListImmediate& Commands, FRHITexture* Material,
			FRHITexture* Normals, FRHITexture* Surface, FRHITexture* Emissive, FRHITexture* Depth) -> void
		{
			const std::array Textures{Material, Normals, Surface, Emissive, Depth};
			for (size_t I = 0; I < Textures.size(); ++I)
			{
				auto* Source = Textures[I];
				if (I == 4)
				{
					auto Copy = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("CullingDepth", Source->GetSizeX(), Source->GetSizeY(), EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
					ASSERT_NE(Copy, nullptr);
					const FRHITextureSubresourceRange DepthRange{ERHITextureAspect::Depth, 0, 1, 0, 1};
					const FRHITextureSubresourceRange ColorRange{ERHITextureAspect::Color, 0, 1, 0, 1};
					Commands.TransitionTextures(std::array{FRHITextureTransition{Source, DepthRange,
						ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead},
						FRHITextureTransition{Copy, ColorRange, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
					const uint32 Bytes = Source->GetSizeX() * Source->GetSizeY() * sizeof(uint32);
					auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create("CullingDepthBits", Bytes,
						sizeof(uint32), EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy));
					ASSERT_NE(Buffer, nullptr);
					Commands.TransitionBuffers(std::array{FRHIBufferTransition{Buffer, 0, Bytes, ERHIAccess::None, ERHIAccess::ComputeShaderReadWrite}});
					auto BufferView = GDynamicRHI->RHICreateBufferView(Buffer, {.Offset = 0, .Size = Bytes, .Type = ERHIBufferViewType::StructuredStorage});
					ASSERT_NE(BufferView, nullptr);
					Commands.SwitchPipeline(ERHIPipeline::Compute);
					Commands.SetComputePipelineState(*GCullingDepthPipeline);
					const std::array Parameters{FRHIShaderParameterResource{.Resource = Source, .BindingIndex = 0, .Type = ERHIBindingType::Texture},
						FRHIShaderParameterResource{.Resource = BufferView.GetReference(), .BindingIndex = 1, .Type = ERHIBindingType::StorageBuffer}};
					Commands.SetShaderParameters(GCullingDepthShader, Parameters);
					Commands.Dispatch((Source->GetSizeX() + 7) / 8, (Source->GetSizeY() + 7) / 8, 1);
					Commands.SwitchPipeline(ERHIPipeline::Graphics);
					Commands.TransitionBuffers(std::array{FRHIBufferTransition{Buffer, 0, Bytes, ERHIAccess::ComputeShaderReadWrite,
						ERHIAccess::TransferRead}});
					Commands.CopyBufferToTexture(Buffer, Copy, std::array{FRHIBufferTextureCopyRegion{
						.TextureExtent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
					GCullingAttachments[I] = Commands.EnqueueTextureReadback(Copy);
					Commands.TransitionTextures(std::array{FRHITextureTransition{Source, DepthRange,
						ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead}});
					continue;
				}
				const auto Range = FRHITextureSubresourceRange{I == 4 ? ERHITextureAspect::Depth : ERHITextureAspect::Color, 0, 1, 0, 1};
				const auto Access = ERHIAccess::GraphicsShaderRead;
				auto Copy = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("CullingAttachment", Source->GetSizeX(), Source->GetSizeY(), Source->GetFormat())
					.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
				ASSERT_NE(Copy, nullptr);
				Commands.TransitionTextures(std::array{FRHITextureTransition{Source, Range, Access, ERHIAccess::TransferRead},
					FRHITextureTransition{Copy, Range, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
				Commands.CopyTexture(Source, Copy, std::array{FRHITextureCopyRegion{
					.SourceAspect = Range.Aspects, .DestinationAspect = Range.Aspects,
					.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1}}});
				GCullingAttachments[I] = Commands.EnqueueTextureReadback(Copy);
				Commands.TransitionTextures(std::array{FRHITextureTransition{Source, Range, ERHIAccess::TransferRead, Access}});
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

	inline auto QualifySceneRenderRoutes(bool bPresentation, bool bGPUCullingQualification = false, bool bAliasingQualification = false,
		std::function<void(bool)> ObservePreparedDraw = {}) -> void
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
		EXPECT_NO_THROW(World->InitializeSubsystems());
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
		bool bObserveTelemetry = true;
		bool bOverrideDuringRecording = false;
		bool bEditorAssistance = false;
		bool bDisableFrustumCulling = false;
		FVector3 WorldShift{};
		uint32 OffscreenOrdinal = 0;
		FSceneViewStateId HistoryId;
		auto RenderOffscreen = [&Renderer, Scene, &ExtraPolicy, &ExtraOptions, &bContactShadows, &OffscreenOrdinal, &bDisableFrustumCulling, &WorldShift, &bObserveTelemetry, &bOverrideDuringRecording, &bEditorAssistance, &HistoryId, ObservePreparedDraw](
								   bool bForceFragment, bool bAmbientOcclusion = true,
			EGroundTruthAmbientOcclusionQuality AOQuality = EGroundTruthAmbientOcclusionQuality::HalfResolution,
			ERenderMode RenderMode = ERenderMode::Lit,
			ESceneRasterFailure Failure = ESceneRasterFailure::None,
			uint32 Width = 96, uint32 Height = 64, ERasterMode RasterMode = ERasterMode::Solid,
			FRenderResourcePreparation Preparation = FRenderResourcePreparation::Create()
							   ) {
			const uint32 PublicationsBefore = GSceneCloudTelemetryPublications;
			auto Pixels = std::make_shared<Durin::FByteBuffer>();
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable
			);
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			do
			{
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Scene, Pixels, Result, bForceFragment, bAmbientOcclusion, AOQuality, RenderMode, RasterMode,
					Failure, Width, Height, Policy = ExtraPolicy, Options = ExtraOptions, bContactShadows, bDisableFrustumCulling, WorldShift, HistoryId, bEditorAssistance, bOverrideDuringRecording, ObservePreparedDraw, Preparation](
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
					View.ViewStateId = HistoryId;
					if (bEditorAssistance) View.OverlayPrimitives.push_back({EViewOverlayShape::Arrow});
					View.ViewLocation += WorldShift;
					View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Math::Normalize(FVector3{1, 0, 0.1}));
					View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
					if (bDisableFrustumCulling) View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
					View.Settings.Mode.RenderMode = RenderMode;
					View.Settings.Mode.RasterMode = RasterMode;
					View.Settings.AmbientOcclusion.bEnabled = bAmbientOcclusion;
					View.Settings.AmbientOcclusion.Quality = AOQuality;
					View.Settings.DirectionalShadow.bEnableContactShadows = bContactShadows;
					auto SelectedPolicy = Policy;
					SelectedPolicy.bForceFragmentVolumetricCloud = bForceFragment;
					SelectedPolicy.RasterFailure = Failure;
					FScopedRendererQualificationPolicy Qualification(SelectedPolicy);
					const bool bDeliberatelyPending = Policy.PendingFeature != EScenePreparationPending::None || Policy.PendingPrimitive.has_value() || (Policy.PendingShadowPrimitive.has_value() && RenderMode == ERenderMode::Lit);
					const bool Prepare = Failure == ESceneRasterFailure::None;
					Renderer.BeginFrame_RenderThread(CommandList);
					if (Prepare)
					{
						const auto DrawsBefore = CommandList.GetNumRecordedDrawCommands();
						Renderer.PrepareViewResources(CommandList, {Scene, View, Output, false, Options}, Preparation);
						EXPECT_EQ(CommandList.GetNumRecordedDrawCommands(), DrawsBefore);
						const auto State = Preparation.GetProgress().State;
						if (bDeliberatelyPending) EXPECT_EQ(State, ERenderResourcePreparationState::Pending);
						if (State != ERenderResourcePreparationState::Ready && !bDeliberatelyPending)
						{
							*Result = State == ERenderResourcePreparationState::Pending
								? ERenderViewResult::RendererResourcesPending : ERenderViewResult::RendererResourcesUnavailable;
							GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
							return;
						}
						if (!bDeliberatelyPending && ObservePreparedDraw) ObservePreparedDraw(true);
					}
					if (bOverrideDuringRecording) SetSceneGraphAuthoringTimingSink(OverrideQualificationAfterAuthoring);
					std::chrono::steady_clock::time_point FrameStart;
					if (Policy.bMeasureLocalLights)
					{
						GLocalLightTimingQueries = {};
						CommandList.BlockUntilGPUIdle();
						FrameStart = std::chrono::steady_clock::now();
					}
					*Result = Renderer.RenderView(CommandList, {Scene, View, Output, false, Options});
					if (bOverrideDuringRecording)
					{
						EXPECT_NE(GRecordingQualificationOverride, nullptr);
						GRecordingQualificationOverride.reset();
						SetSceneGraphAuthoringTimingSink(nullptr);
					}
					if (Prepare && !bDeliberatelyPending && ObservePreparedDraw) ObservePreparedDraw(false);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					if (Policy.bMeasureLocalLights)
					{
						CommandList.BlockUntilGPUIdle();
						GLocalLightFrameNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - FrameStart).count();
					}
					if (*Result == ERenderViewResult::Success)
					{
						GDynamicRHI->RHIReadTexture2D(CommandList, Output, 0, 0, *Pixels);
						CommandList.PollTextureReadbacks();
						CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					}
				}
			);
			FlushRenderingCommands();
				if (*Result != ERenderViewResult::RendererResourcesPending) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < Deadline);
			const auto Expected = ExtraPolicy.bFailLocalLightAllocation ? ERenderViewResult::RendererResourcesUnavailable : Failure == ESceneRasterFailure::HybridBootstrap || Failure == ESceneRasterFailure::Forward ? ERenderViewResult::RequiredEnvironmentUnavailable : Failure != ESceneRasterFailure::None ? ERenderViewResult::RendererResourcesUnavailable :
																																																						  ERenderViewResult::Success;
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
				EXPECT_EQ(GSceneCloudTelemetryPublications, PublicationsBefore + (bObserveTelemetry ? 1 : 0));
				EXPECT_EQ(Pixels->size(), static_cast<size_t>(Width) * Height * 4);
				if (ExtraPolicy.bMeasureLocalLights) return Pixels;
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
		// Missing optional weather still imports the normalized white fallback.
		ASSERT_EQ(Component->GetWeatherTexture(), nullptr);
		const auto Weather = std::ranges::find_if(GSceneCloudGraphCaptures.back().Resources,
			[](const auto& Resource) { return Resource.Name == "Scene.VolumetricCloud.Weather"; });
		ASSERT_NE(Weather, GSceneCloudGraphCaptures.back().Resources.end());
		EXPECT_TRUE(Weather->bExternal);
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
				const auto Preparation = FRenderResourcePreparation::Create();
				const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				do
				{
				EnqueueRenderCommand<FSceneCloudRender>(
					[&Renderer, Scene, Viewport, Width, Height, Result, bForceFragment, Preparation](
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
							Renderer.BeginFrame_RenderThread(CommandList);
							const FSceneViewRenderRequest Request{Scene, View, BackBuffer, true, {}};
							Renderer.PrepareViewResources(CommandList, Request, Preparation);
							const auto State = Preparation.GetProgress().State;
							*Result = State == ERenderResourcePreparationState::Pending ? ERenderViewResult::RendererResourcesPending
								: State == ERenderResourcePreparationState::Ready ? Renderer.RenderView(CommandList, Request)
								: ERenderViewResult::RendererResourcesUnavailable;
						}
						if (*Result != ERenderViewResult::Success && BackBuffer)
							CommandList.TransitionTextures(std::array{FRHITextureTransition::Whole(
								BackBuffer, ERHIAccess::Discard, ERHIAccess::Present
							)});
						// Preparation can span frames. Present each acquired image even while
						// drawing is Pending, otherwise the test exhausts swapchain acquisition.
						CommandList.EndDrawingViewport(Viewport, true, false);
						GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					}
				);
				FlushRenderingCommands();
					if (*Result != ERenderViewResult::RendererResourcesPending) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				} while (std::chrono::steady_clock::now() < Deadline);
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
		auto* TranslucentMaterial = MakeRouteMaterial("SceneRouteTranslucent", EMaterialShadingModel::Lit, EMaterialBlendMode::Translucent);
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
		LOD.IndexBuffer.Init(bGPUCullingQualification ? std::vector<uint32>{0, 0, 0, 0, 1, 2, 0, 2, 3} : std::vector<uint32>{0, 1, 2, 0, 2, 3});
		LOD.Sections.push_back({.Name = "Route", .FirstIndex = bGPUCullingQualification ? 3u : 0u, .IndexCount = 6,
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
				std::vector<FMaterialRenderProxyRef>{LitMaterial->GetRenderProxy()}),
			Math::TranslationMatrix(FVector3{100, -35, 2510}));
		FlushRenderingCommands();

		const auto HasPass = [](const char* Name) {
			return std::ranges::any_of(GSceneCloudGraphCaptures.back().Passes,
				[&](const auto& Pass) { return Pass.Name == Name; });
		};
		const auto RenderUnlit = [&] {
			return RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit);
		};
		static FByteBuffer LocalPixels;
		auto CaptureLocalLighting = +[](FRHICommandListImmediate& Commands, FRHITexture* Target) {
			const auto Texture = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("LocalLightingReadback", Target->GetSizeX(), Target->GetSizeY(), Target->GetFormat())
				.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback));
			ASSERT_TRUE(Texture);
			const FRHITextureSubresourceRange Whole{ERHITextureAspect::Color, 0, 1, 0, 1};
			Commands.TransitionTextures(std::array{FRHITextureTransition{Target, Whole, ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
				FRHITextureTransition{Texture, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			Commands.CopyTexture(Target, Texture, std::array{FRHITextureCopyRegion{.Extent = {Target->GetSizeX(), Target->GetSizeY(), 1}}});
			Commands.TransitionTextures(std::array{FRHITextureTransition{Target, Whole, ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead}});
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Texture, 0, 0, LocalPixels));
		};
		// Save the same view outputs on either side of a lighting ABI change.
		if (const auto* Directory = std::getenv("DURIN_LOCAL_LIGHT_CAPTURES"))
		{
			std::filesystem::create_directories(Directory);
			const auto Program = LitMaterial->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program);
			const auto& Source = Program->GetDebugData().GeneratedSource;
			if (!Source.empty())
			{
				std::ofstream Shader(std::filesystem::path(Directory) / "ForwardMaterial.slang");
				Shader << Source;
				ASSERT_TRUE(Shader.good());
			}
			std::vector<FLightSceneProxy*> Lights;
			for (uint32 Count : {0u, 1u, 4u})
			{
				while (Lights.size() < Count)
				{
					const auto Index = static_cast<uint32>(Lights.size());
					const FLightComponentId Id(23000 + Index);
					if (Index % 2)
						Lights.push_back(PublishLightForTest<FSpotLightSceneProxy>(*Scene, Id,
							FSpotLightSceneData{.Position = {135, -35.0 + Index, 2510}, .Direction = {-1, 0, 0}, .Intensity = 1800, .Range = 200}));
					else
						Lights.push_back(PublishLightForTest<FPointLightSceneProxy>(*Scene, Id,
							FPointLightSceneData{.Position = {135, -35.0 + Index, 2510}, .Intensity = 1100, .Range = 200}));
					ASSERT_TRUE(Lights.back());
				}
				ExtraPolicy.ObserveAdmittedDraws = [Count](const FSceneRenderPlan& Plan) {
					ASSERT_EQ(Plan.Lighting.Lights.Local.size(), Count);
					for (uint32 Index = 0; Index < Count; ++Index)
						EXPECT_EQ(Plan.Lighting.Lights.Local[Index].Id.Value, 23000u + Index);
				};
				auto Save = [&](const char* Route, const auto& Pixels) {
					ASSERT_EQ(Pixels->size(), 96u * 64u * (std::string_view(Route) == "local" ? 8u : 4u));
					EXPECT_EQ(GSceneCloudTelemetry.Lighting.SelectedPointLights + GSceneCloudTelemetry.Lighting.SelectedSpotLights, Count);
					std::ofstream File(std::filesystem::path(Directory) / (std::to_string(Count) + "-" + Route + ".rgba"), std::ios::binary);
					File.write(reinterpret_cast<const char*>(Pixels->data()), Pixels->size());
					ASSERT_TRUE(File.good());
				};
				Save("final", RenderOffscreen(false, false));
				ExtraOptions.DeferredDirectionalDebugMode = EDeferredDirectionalDebugMode::Local;
				LocalPixels.clear();
				SetDeferredDirectionalCaptureSink(CaptureLocalLighting);
				RenderUnlit();
				SetDeferredDirectionalCaptureSink(nullptr);
				Save("local", &LocalPixels);
				ExtraOptions = {};
				Save("forward", RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, 96, 64, ERasterMode::Wireframe));
			}
			ExtraPolicy.ObserveAdmittedDraws = {};
			for (auto* Light : Lights) FSceneInterfaceTestAccess::TryRemoveLightProxy(*Scene, Light);
			FlushRenderingCommands();
		}
		// Compare the whole receiver against the same spot rendered alone. All filler
		// lights remain inside the view but their spheres miss the receiver.
		enum class ELightReceiver { DeferredLocal, ForwardWireframe, LitTranslucent };
		auto CaptureReceiver = [&](ELightReceiver Route) {
			if (Route == ELightReceiver::ForwardWireframe)
				return *RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, 96, 64, ERasterMode::Wireframe);
			if (Route == ELightReceiver::LitTranslucent) return *RenderOffscreen(false, false);
			LocalPixels.clear();
			ExtraOptions.DeferredDirectionalDebugMode = EDeferredDirectionalDebugMode::Local;
			SetDeferredDirectionalCaptureSink(CaptureLocalLighting);
			RenderUnlit();
			SetDeferredDirectionalCaptureSink(nullptr);
			ExtraOptions = {};
			return LocalPixels;
		};
		auto SetReceiver = [&](DMaterial* Material) {
			FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10001),
				std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
					std::vector<FMaterialRenderProxyRef>{Material->GetRenderProxy()}),
				Math::TranslationMatrix(FVector3{100, -35, 2510}));
			FlushRenderingCommands();
		};
		for (auto Route : {ELightReceiver::DeferredLocal, ELightReceiver::ForwardWireframe, ELightReceiver::LitTranslucent})
		{
			SCOPED_TRACE(static_cast<int>(Route));
			SetReceiver(Route == ELightReceiver::LitTranslucent ? TranslucentMaterial : LitMaterial);
			auto Publish = [&](uint32 Index, bool Hits, float OuterCone = 40.f, double DirectionX = -1.0) -> FLightSceneProxy* {
				const FLightComponentId Id(24000 + Index);
				if (!Hits && Index % 2)
					return PublishLightForTest<FPointLightSceneProxy>(*Scene, Id,
						FPointLightSceneData{.Position = {300, 0, 2510}, .Intensity = 1500, .Range = 6});
				return PublishLightForTest<FSpotLightSceneProxy>(*Scene, Id,
					FSpotLightSceneData{.Position = Hits ? FVector3{135,-35,2510} : FVector3{300,0,2510},
						.Direction = {DirectionX,0,0}, .Intensity = 5000, .Range = Hits ? 100.f : 6.f,
						.InnerConeAngle = 15, .OuterConeAngle = OuterCone});
			};
			auto Remove = [&](FLightSceneProxy* Light) {
				FSceneInterfaceTestAccess::TryRemoveLightProxy(*Scene, Light);
				FlushRenderingCommands();
			};
			ExtraPolicy.LocalLightRecordBudget = 32;
			const auto Dark = CaptureReceiver(Route);
			auto* Fifth = Publish(4, true);
			const auto Reference = CaptureReceiver(Route);
			EXPECT_NE(Dark, Reference);
			Remove(Fifth);
			// A hard 15-degree edge cuts pixels from the soft 15-to-40 degree cone.
			Fifth = Publish(4, true, 15);
			const auto HardCone = CaptureReceiver(Route);
			EXPECT_NE(HardCone, Reference);
			EXPECT_NE(HardCone, Dark);
			Remove(Fifth);
			Fifth = Publish(4, true, 40, 1);
			EXPECT_EQ(CaptureReceiver(Route), Dark);
			Remove(Fifth);
			Fifth = Publish(4, true);
			std::vector<FLightSceneProxy*> Fillers;
			for (uint32 Count : {5u, 8u, 32u, 33u})
			{
				for (uint32 Index = static_cast<uint32>(Fillers.size()); Fillers.size() < Count - 1; ++Index)
				{
					const uint32 Id = Index >= 4 ? Index + 1 : Index;
					Fillers.push_back(Publish(Id, Id == 32));
				}
				const auto Actual = CaptureReceiver(Route);
				EXPECT_EQ(Actual, Reference) << Count;
				const auto& L = GSceneCloudTelemetry.Lighting;
				EXPECT_EQ(L.VisiblePointLights + L.VisibleSpotLights, Count);
				EXPECT_EQ(L.SelectedPointLights + L.SelectedSpotLights, std::min(Count, 32u));
				EXPECT_EQ(L.OverflowPointLights + L.OverflowSpotLights, Count == 33 ? 1u : 0u);
				EXPECT_EQ(L.LocalRecordUploadBytes, 64u * std::min(Count, 32u));
				if (Count == 5)
				{
					ExtraPolicy.LocalLightRecordBudget = 4;
					EXPECT_EQ(CaptureReceiver(Route), Dark);
					ExtraPolicy.LocalLightRecordBudget = 32;
					Remove(Fifth);
					EXPECT_EQ(CaptureReceiver(Route), Dark);
					Fifth = Publish(4, true);
				}
			}
			// Removing light five admits the previously over-budget light 33. Its identical cone must now reproduce the single-light pixels.
			Remove(Fifth);
			EXPECT_EQ(CaptureReceiver(Route), Reference);
			for (auto* Light : Fillers) Remove(Light);
		}
		ExtraPolicy.LocalLightRecordBudget.reset();
		SetReceiver(LitMaterial);
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10002),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{UnlitMaterial->GetRenderProxy()}),
			Math::TranslationMatrix(FVector3{100, 35, 2510}));
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10003),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{MaskedMaterial->GetRenderProxy()}),
			Math::TranslationMatrix(FVector3{95, -35, 2520}));
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10004),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{TranslucentMaterial->GetRenderProxy()}),
			Math::TranslationMatrix(FVector3{90, 35, 2510}));
		FlushRenderingCommands();

		// Optional hardware characterization. Paired budgets alternate order each frame.
		if (const char* Path = std::getenv("DURIN_LOCAL_LIGHT_SAMPLES"))
		{
			std::ofstream Samples(Path);
			ASSERT_TRUE(Samples.good());
			Samples << "layout,count,budget,sample,gather_ns,pack_ns,upload_bytes,allocation_bytes,completed_frame_wall_ns,deferred_ns,forward_translucent_ns\n";
			SetSceneRenderGraphCaptureSink(nullptr);
			SetDeferredDirectionalTimingQuerySink(+[](const FGPUTimingQueryRHIRef& Q) { GLocalLightTimingQueries[0] = Q; });
			const auto CaptureForwardTime = +[](const FGPUTimingQueryRHIRef& Q) { GLocalLightTimingQueries[1] = Q; };
			SetSortedTranslucencyTimingQuerySink(CaptureForwardTime);
			ExtraPolicy.bMeasureLocalLights = true;
			for (bool Dense : {false, true})
			{
				std::vector<FLightSceneProxy*> Lights;
				for (uint32 Count : {0u, 4u, 8u, 32u})
				{
					while (Lights.size() < Count)
					{
						const uint32 I = static_cast<uint32>(Lights.size());
						const FVector3 Position{120, Dense ? -10.0 : -75.0 + (I % 8) * 20, Dense ? 2510.0 : 2480.0 + (I / 8) * 20};
						const float Range = Dense ? 200.f : 26.f;
						const FLightComponentId Id(25000 + I);
						if (I % 2)
							Lights.push_back(PublishLightForTest<FSpotLightSceneProxy>(*Scene, Id,
								FSpotLightSceneData{.Position = Position, .Direction = {-1,0,0}, .Intensity = 120, .Range = Range}));
						else
							Lights.push_back(PublishLightForTest<FPointLightSceneProxy>(*Scene, Id,
								FPointLightSceneData{.Position = Position, .Intensity = 120, .Range = Range}));
					}
					FlushRenderingCommands();
					SetSortedTranslucencyTimingQuerySink(nullptr);
					ExtraPolicy.bMeasureLocalLights = false;
					ExtraPolicy.LocalLightRecordBudget = 32;
					const auto Untimed = RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
						ERenderMode::Lit, ESceneRasterFailure::None, 1920, 1080);
					SetSortedTranslucencyTimingQuerySink(CaptureForwardTime);
					ExtraPolicy.bMeasureLocalLights = true;
					for (int Sample = -4; Sample < 32; ++Sample)
					{
						FByteBuffer First;
						for (uint32 Pair = 0; Pair < 2; ++Pair)
						{
							const uint32 Budget = ((Sample + 4 + Pair) % 2 == 0) ? 4 : 32;
							ExtraPolicy.LocalLightRecordBudget = Budget;
							const auto Image = RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
								ERenderMode::Lit, ESceneRasterFailure::None, 1920, 1080);
							const auto& L = GSceneCloudTelemetry.Lighting;
							EXPECT_EQ(L.VisiblePointLights + L.VisibleSpotLights, Count);
							EXPECT_EQ(L.LocalRecordUploadBytes, std::min(Count, Budget) * 64u);
							if (Budget == 32 && Sample == -4) EXPECT_EQ(*Untimed, *Image);
							if (Count <= 4 && Sample == 0)
							{
								if (!Pair) First = *Image;
								else EXPECT_EQ(First, *Image);
							}
							if (Sample < 0) continue;
							Samples << (Dense ? "dense" : "sparse") << ',' << Count << ',' << Budget << ',' << Sample
								<< ',' << L.GatherNanoseconds << ',' << L.PackNanoseconds << ',' << L.LocalRecordUploadBytes
								<< ',' << L.LocalDeviceAllocationBytes << ',' << GLocalLightFrameNanoseconds;
							for (const auto& Query : GLocalLightTimingQueries)
							{
								ASSERT_TRUE(Query);
								const auto Result = Query->GetResult();
								ASSERT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
								Samples << ',' << Result.DurationNanoseconds;
							}
							Samples << '\n';
						}
					}
				}
				for (auto* Light : Lights) FSceneInterfaceTestAccess::TryRemoveLightProxy(*Scene, Light);
				FlushRenderingCommands();
			}
			GLocalLightTimingQueries = {};
			ExtraPolicy = {};
			SetDeferredDirectionalTimingQuerySink(nullptr);
			SetSortedTranslucencyTimingQuerySink(nullptr);
			SetSceneRenderGraphCaptureSink(CaptureSceneCloudGraph);
		}

		const auto UnlitWithoutLocal = RenderUnlit();
		const auto LitWithoutLocal = RenderOffscreen(false, false);
		auto* Point = PublishLightForTest<FPointLightSceneProxy>(*Scene, FLightComponentId(22001),
			FPointLightSceneData{.Position = {60, -35, 2510}, .Intensity = 5000.0f, .Range = 200.0f});
		auto* Spot = PublishLightForTest<FSpotLightSceneProxy>(*Scene, FLightComponentId(22002),
			FSpotLightSceneData{.Position = {135, -35, 2510}, .Direction = {-1, 0, 0}, .Intensity = 5000.0f, .Range = 200.0f});
		ASSERT_TRUE(Point && Spot);
		EXPECT_EQ(*UnlitWithoutLocal, *RenderUnlit());
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.LocalRecordUploadBytes, 0u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SubmittedPointLights, 0u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SubmittedSpotLights, 0u);
		ExtraPolicy.bFailLocalLightAllocation = true;
		RenderOffscreen(false, false);
		ExtraPolicy.bFailLocalLightAllocation = false;
		EXPECT_NE(*LitWithoutLocal, *RenderOffscreen(false, false));
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.LocalRecordUploadBytes, 128u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SelectedPointLights, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SelectedSpotLights, 1u);
		const auto& Graph = GSceneCloudGraphCaptures.back();
		const auto Buffer = std::ranges::find(Graph.Resources, "Scene.LocalLights", &FRDGResourceCapture::Name);
		ASSERT_NE(Buffer, Graph.Resources.end());
		uint32 Writes = 0;
		std::vector<std::string> Readers;
		for (const auto& Use : Graph.Uses)
		{
			if (Use.ResourceId != Buffer->ResourceId) continue;
			EXPECT_EQ(Use.BufferSize, 128u);
			if (Use.Use == ERDGUse::Write) ++Writes;
			if (Use.Use == ERDGUse::Read)
				for (const auto& Pass : Graph.Passes)
					if (Pass.DeclarationIndex == Use.PassDeclarationIndex) Readers.push_back(Pass.Name);
		}
		EXPECT_EQ(Writes, 1u);
		EXPECT_NE(std::ranges::find(Readers, "Scene.ProductionDeferred"), Readers.end());
		EXPECT_NE(std::ranges::find(Readers, "Scene.Color"), Readers.end());
		ExtraOptions.DeferredDirectionalDebugMode = EDeferredDirectionalDebugMode::Local;
		RenderUnlit();
		EXPECT_TRUE(HasPass("Scene.DeferredDirectionalLighting"));
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.LocalRecordUploadBytes, 128u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SelectedPointLights, 1u);
		EXPECT_EQ(GSceneCloudTelemetry.Lighting.SelectedSpotLights, 1u);
		ExtraOptions = {};
		FSceneInterfaceTestAccess::TryRemoveLightProxy(*Scene, Point);
		FSceneInterfaceTestAccess::TryRemoveLightProxy(*Scene, Spot);
		FlushRenderingCommands();

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

		// The presentation smoke also runs on devices limited to direct submission.
		if (bGPUCullingQualification && (!bPresentation || GDynamicRHI->RHIGetCapabilities()->bSupportsIndirectDraw))
		{
			FShaderCompileOptions DepthOptions;
			DepthOptions.Target = std::string_view(Backend ? Backend : "") == "metal" ? MetalShaderTarget : VulkanShaderTarget;
			DepthOptions.EntryPoints = {"ComputeMain"};
			DepthOptions.Frequencies = {EShaderFrequency::Compute};
			const auto CullCode = FShaderCompilingManager::Get().CompileMountedSync("/Engine/InstanceCulling", [&] { auto C = DepthOptions; C.EntryPoints = {"CullMain"}; return C; }());
			ASSERT_TRUE(CullCode) << FormatShaderCompilerErrors(CullCode.Errors);
			{
				std::ofstream Output(GSceneCloudEvidenceRoot / "culling-native-code.bin", std::ios::binary);
				const auto Bytes = CullCode.CompiledShaders[0].Code.GetBytes();
				Output.write(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			}
			{
				auto Options = DepthOptions; Options.EntryPoints = {"VertexMain"}; Options.Frequencies = {EShaderFrequency::Vertex};
				const auto Code = FShaderCompilingManager::Get().CompileMountedSync("/Engine/StaticMeshBasePass", Options);
				ASSERT_TRUE(Code) << FormatShaderCompilerErrors(Code.Errors);
				std::ofstream Output(GSceneCloudEvidenceRoot / "culling-vertex-native.bin", std::ios::binary);
				const auto Bytes = Code.CompiledShaders[0].Code.GetBytes();
				Output.write(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			}
			const auto DepthCode = FShaderCompilingManager::Get().CompileMountedSync("/Engine/Qualification/DepthObservation", DepthOptions);
			ASSERT_TRUE(DepthCode) << FormatShaderCompilerErrors(DepthCode.Errors);
			EnqueueRenderCommand<FSceneCloudRender>([&](FRHICommandListImmediate&) {
				GCullingDepthShader = GDynamicRHI->RHICreateShader(MakeShaderCreateDesc(DepthCode.CompiledShaders[0]));
				ASSERT_NE(GCullingDepthShader, nullptr);
				FComputePipelineStateInitializer Initializer;
				Initializer.ComputeShader = GCullingDepthShader;
				ASSERT_TRUE(BuildPipelineLayoutFromShaders(DepthCode.CompiledShaders, Initializer.PipelineLayout));
				GCullingDepthPipeline = GDynamicRHI->RHICreateComputePipelineState("CullingDepthObservation", Initializer);
				ASSERT_NE(GCullingDepthPipeline, nullptr);
			});
			FlushRenderingCommands();
			SetGBufferCaptureSink(CaptureCullingAttachments);
			bool bBoundary = false;
			auto InstallGroups = [&](uint32 VisiblePerGroup, bool TwoGroups) {
				for (uint32 Group = 0; Group < (TwoGroups ? 2u : 1u); ++Group)
				{
					auto Instances = std::make_shared<std::vector<FStaticMeshRenderInstance>>();
					for (uint32 Index = 0; Index < 64; ++Index)
					{
						// Straddle the frustum even in the zero-visible layout so CPU
						// aggregate classification leaves the individual tests to compute.
						const double Y = Index < VisiblePerGroup
							? (bBoundary ? 100.0 : -35.0 + Group * 70 + (Index % 8) * 2)
							: (Index % 2 ? 1000000.0 : -1000000.0);
						const FVector3 Position{bBoundary ? 100.0 : 100.0 + Index * 3 + Group * 5,
							Y, bBoundary ? 2510.0 : 2510.0 + (Index / 8) * 2};
						Instances->push_back({.Id = Index + 1, .LocalToPrimitive = Math::TranslationMatrix(Position)});
					}
					EXPECT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(Group == 0 ? 10001 : 10003),
						std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector<FMaterialRenderProxyRef>{
							(Group == 0 ? LitMaterial : MaskedMaterial)->GetRenderProxy()}, Instances), Math::TranslationMatrix(WorldShift)));
				}
				FlushRenderingCommands();
			};
			auto TakeAttachments = [] {
				std::array<FByteBuffer, 5> Result;
				for (size_t I = 0; I < Result.size(); ++I)
				{
					EXPECT_NE(GCullingAttachments[I], nullptr);
					if (!GCullingAttachments[I]) continue;
					EXPECT_EQ(GCullingAttachments[I]->GetState(), ERHITextureReadbackState::Ready) << I;
					EXPECT_TRUE(GCullingAttachments[I]->TakePixels(Result[I]));
					EXPECT_FALSE(Result[I].empty()) << I;
				}
				return Result;
			};
			auto CheckParity = [&](uint32 VisiblePerGroup, bool TwoGroups, bool Async, uint32 Width = 96) {
				SCOPED_TRACE(::testing::Message() << "boundary=" << bBoundary << " visible=" << VisiblePerGroup << " two=" << TwoGroups << " async=" << Async << " width=" << Width);
				InstallGroups(VisiblePerGroup, TwoGroups);
				ExtraOptions.bEnableGPUCulling = false;
				ExtraOptions.bEnableAsyncCompute = Async;
				const auto Direct = RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, Width);
				const auto DirectAttachments = TakeAttachments();
				ExtraOptions.bEnableGPUCulling = true;
				const auto Indirect = RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, Width);
				EXPECT_EQ(*Direct, *Indirect);
				const auto IndirectAttachments = TakeAttachments();
				for (size_t A = 0; A < DirectAttachments.size(); ++A)
				{
					const auto& D = DirectAttachments[A];
					const auto& G = IndirectAttachments[A];
					EXPECT_EQ(D.size(), G.size());
					size_t Differences = 0;
					for (size_t I = 0; I < std::min(D.size(), G.size()); ++I) Differences += D[I] != G[I];
					EXPECT_EQ(Differences, 0u) << "attachment=" << A;
					for (const auto& [Name, Bytes] : {std::pair{"direct", &D}, std::pair{"indirect", &G}})
					{
						std::ofstream Output(GSceneCloudEvidenceRoot / ("attachment-" + std::to_string(OffscreenOrdinal)
							+ "-" + std::to_string(A) + "-" + Name + ".bin"), std::ios::binary);
						Output.write(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
					}
				}
				const auto& T = GSceneCloudTelemetry.GBuffer;
				const uint32 Groups = TwoGroups ? 2 : 1;
				EXPECT_EQ(T.GPUCullingDispatchedViews, 1u);
				EXPECT_EQ(T.GPUCullingFallbackViews, 0u);
				EXPECT_EQ(T.GPUCullingCandidates, Groups * 64);
				EXPECT_EQ(T.GPUCullingGroups, Groups);
				EXPECT_EQ(T.GPUCullingScheduledGraphicsQueueViews, Async ? 0u : 1u);
				EXPECT_EQ(T.GPUCullingPhysicalGraphicsQueueViews, 1u);
				EXPECT_EQ(T.GPUCullingPhysicalComputeQueueViews, 0u);
				EXPECT_EQ(T.GPUCullingScheduledAsyncQueueViews, Async ? 1u : 0u);
				EXPECT_EQ(T.GPUCullingIndirectDraws, Groups);
				ASSERT_NE(T.GPUCullingVisibility, nullptr);
				ASSERT_EQ(T.GPUCullingVisibility->GetState(), ERHITextureReadbackState::Ready);
				const auto Counts = T.GPUCullingVisibility->GetCounts();
				ASSERT_TRUE(Counts);
				const uint32 ExpectedVisible = Groups * (bDisableFrustumCulling ? 64u : VisiblePerGroup);
				EXPECT_EQ(Counts->Visible, ExpectedVisible);
				EXPECT_EQ(Counts->Culled, Groups * 64u - ExpectedVisible);
				std::cout << "GPU_CULLING_PARITY candidates=" << Counts->Candidates << ",visible=" << Counts->Visible
					<< ",culled=" << Counts->Culled << ",groups=" << Groups << ",async_requested=" << Async << '\n';
			};
			bDisableFrustumCulling = true;
			CheckParity(32, false, false);
			bDisableFrustumCulling = false;
			CheckParity(0, false, false);
			CheckParity(32, false, false);
			CheckParity(64, false, true);
			bBoundary = true;
			CheckParity(64, false, true);
			bBoundary = false;
			CheckParity(32, true, true);
			CheckParity(64, true, false, 112);
			CheckParity(64, true, false, 96); // pool reuse after resize
			for (uint32 Repeat = 0; Repeat < 4; ++Repeat) CheckParity(Repeat % 2 ? 64 : 0, true, Repeat % 2 != 0);
			WorldShift = FVector3{1e12, -1e12, 1e12};
			CheckParity(32, true, true);
			WorldShift = {};
			InstallGroups(32, true);
			ExtraOptions.bEnableGPUCulling = false;
			const auto Reference = RenderOffscreen(false, false);
			ExtraOptions.bEnableGPUCulling = true;
			bObserveTelemetry = false;
			SetViewRenderTelemetrySink(nullptr);
			SetGBufferCaptureSink(nullptr);
			EXPECT_EQ(*Reference, *RenderOffscreen(false, false));
			EXPECT_FALSE(std::ranges::any_of(GSceneCloudGraphCaptures.back().Resources, [](const auto& Resource) {
				return Resource.Name == "Scene.GBuffer.GPUCulling.Counters";
			}));
			bObserveTelemetry = true;
			SetViewRenderTelemetrySink(CaptureSceneCloudTelemetry);
			SetGBufferCaptureSink(CaptureCullingAttachments);
			for (const auto Failure : {EGPUCullingFailure::ShaderUnavailable, EGPUCullingFailure::PipelineUnavailable,
				EGPUCullingFailure::MetadataBudget, EGPUCullingFailure::CommandAdmission})
			{
				ExtraOptions.bEnableGPUCulling = true;
				ExtraPolicy.GPUCullingFailure = Failure;
				EXPECT_EQ(*Reference, *RenderOffscreen(false, false));
				EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingDispatchedViews, 0u);
				EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingFallbackViews, 1u);
				if (const auto Query = GSceneCloudTelemetry.GBuffer.GPUCullingVisibility)
					EXPECT_EQ(Query->GetState(), ERHITextureReadbackState::Canceled);
			}
			ExtraPolicy = {};
			// Changing the global scope after authoring must affect neither allocation
			// nor the recorded GPU dispatch for this already-frozen view.
			bOverrideDuringRecording = true;
			EXPECT_EQ(*Reference, *RenderOffscreen(false, false));
			EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingDispatchedViews, 1u);
			bOverrideDuringRecording = false;

			for (const auto Cause : {ERendererResourceInvalidationCause::ShaderChanged,
				ERendererResourceInvalidationCause::Device, ERendererResourceInvalidationCause::ManualRetry})
			{
				Renderer.RequestResourceInvalidation(Cause);
				FlushRenderingCommands();
				EXPECT_EQ(*Reference, *RenderOffscreen(false, false));
				EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingDispatchedViews, 1u);
			}
			SetGBufferCaptureSink(nullptr);
			GCullingAttachments = {};
			GCullingDepthShader = nullptr;
			GCullingDepthPipeline = nullptr;
			ExtraOptions = {};
			bDisableFrustumCulling = false;
		}

		if (bAliasingQualification)
		{
			bContactShadows = true;
			for (uint32 Repeat = 0; Repeat < 4; ++Repeat)
			{
				const uint32 Width = Repeat % 2 ? 112 : 96;
				ExtraOptions = {};
				const auto Direct = RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, Width);
				ExtraOptions.bEnableTransientAliasing = true;
				ExtraOptions.bEnableAsyncCompute = Repeat % 2 != 0;
				const bool UnsupportedIndirect = !GDynamicRHI->RHIGetCapabilities()->bSupportsIndirectDraw;
				ExtraOptions.bEnableGPUCulling = UnsupportedIndirect;
				EXPECT_EQ(*Direct, *RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution,
					ERenderMode::Lit, ESceneRasterFailure::None, Width));
				if (UnsupportedIndirect)
				{
					EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingUnsupportedFallbackViews, 1u);
					EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GPUCullingDispatchedViews, 0u);
					EXPECT_GT(GSceneCloudTelemetry.GBuffer.GPUCullingDirectFallbackDraws, 0u);
				}
				const auto& Capture = GSceneCloudGraphCaptures.back();
				std::cout << "SCENE_ALIASING mode=" << (std::getenv("DURIN_RHI_EXECUTION") ? std::getenv("DURIN_RHI_EXECUTION") : "default")
					<< ",native_supported=" << GDynamicRHI->RHISupportsTransientAliasing()
					<< ",handoffs=" << Capture.ExecutionPlan.AliasingHandoffs.size() << ",width=" << Width << '\n';
			}
			ExtraOptions = {};
			bContactShadows = false;
		}

		// Cloud history survives a real later-stage failure even when this
		// stream has no history left after an explicit reset.
		{
			Component->SetEnabled(true);
			auto HistoryOwner = Renderer.CreateViewState();
			HistoryId = HistoryOwner.GetId();
			ASSERT_TRUE(HistoryId.IsValid());
			RenderOffscreen(false);
			Renderer.InvalidateViewState(HistoryId);
			RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution,
				ERenderMode::Lit, ESceneRasterFailure::SortedTranslucency);
			RenderOffscreen(false);
			EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudHistoryAccepted, 1u);
			Component->SetEnabled(false);
			HistoryId = {};
			HistoryOwner.Reset();
			FlushRenderingCommands();
		}

		// Each injected raster failure executes the real authored route and must
		// survive later cloud/sorted stages without becoming a successful publication.
		for (const auto Failure : {ESceneRasterFailure::HybridBootstrap, ESceneRasterFailure::ProductionDeferred,
			ESceneRasterFailure::RetainedForward, ESceneRasterFailure::SortedTranslucency})
			RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Lit, Failure);
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit, ESceneRasterFailure::Forward);

		// The same final Completion must report success and preserve the original
		// upstream error when editor assistance is the output root.
		bEditorAssistance = true;
		RenderOffscreen(false);
		EXPECT_TRUE(std::ranges::any_of(GSceneCloudGraphCaptures.back().Passes,
			[](const auto& Pass) { return Pass.Name == "Scene.EditorAssistance"; }));
		RenderOffscreen(false, true, EGroundTruthAmbientOcclusionQuality::HalfResolution,
			ERenderMode::Lit, ESceneRasterFailure::SortedTranslucency);
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
			ERenderMode::Unlit, ESceneRasterFailure::Forward);

		// Loading/capture owners can keep waiting while the same interactive view
		// produces a complete frame from ready components and feature routes.
		bEditorAssistance = false;
		ExtraPolicy = {};
		const auto WithoutAOPixels = RenderOffscreen(false, false);
		ExtraPolicy.PendingFeature = EScenePreparationPending::AmbientOcclusion;
		EXPECT_EQ(*WithoutAOPixels, *RenderOffscreen(false, true));
		EXPECT_FALSE(HasPass("Scene.AmbientOcclusion"));
		ExtraPolicy = {};
		RenderOffscreen(false, true);
		EXPECT_TRUE(HasPass("Scene.AmbientOcclusion"));

		const auto WithoutCloud = RenderOffscreen(false, false);
		Component->SetEnabled(true);
		RenderOffscreen(false, false);
		ExtraPolicy.PendingFeature = EScenePreparationPending::CloudComposite;
		EXPECT_EQ(*WithoutCloud, *RenderOffscreen(false, false));
		EXPECT_FALSE(HasPass("Scene.VolumetricCloud"));
		ExtraPolicy = {};
		auto LocalHistory = Renderer.CreateViewState();
		HistoryId = LocalHistory.GetId();
		RenderOffscreen(false, false);
		ExtraPolicy.PendingFeature = EScenePreparationPending::CloudTemporal;
		RenderOffscreen(false, false);
		EXPECT_TRUE(HasPass("Scene.VolumetricCloud"));
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudTemporalDraws, 0u);
		ExtraPolicy = {};
		RenderOffscreen(false, false);
		EXPECT_EQ(GSceneCloudTelemetry.VolumetricCloud.VolumetricCloudTemporalDraws, 1u);
		HistoryId = {};
		LocalHistory.Reset();
		Component->SetEnabled(false);

		// Extend this receiver along camera depth so that its bounds cross cascade splits.
		auto OverlapTransform = Math::TranslationMatrix(FVector3{65, -15, 2510});
		OverlapTransform[1][0] = 1.5;
		FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(10001),
			std::make_unique<FStaticMeshSceneProxy>(Geometry.get(),
				std::vector<FMaterialRenderProxyRef>{LitMaterial->GetRenderProxy()}), OverlapTransform);
		FlushRenderingCommands();
		using FMembership = std::array<std::vector<uint64>, 1 + DirectionalShadowCascadeCount>;
		FMembership Admitted;
		ExtraPolicy.ObserveAdmittedDraws = [&](const FSceneRenderPlan& Plan) {
			auto Capture = [](const FPreparedStaticMeshView& View) {
				std::vector<uint64> Ids;
				for (uint32 Index = 0; Index < View.GetNumSections(); ++Index)
					Ids.push_back(View.GetPrimitive(View.GetDraw(Index))->PrimitiveId.Value);
				return Ids;
			};
			Admitted = {};
			Admitted[0] = Capture(Plan.Receiver.StaticMeshes);
			if (Plan.DirectionalShadow)
				for (uint32 Cascade = 0; Cascade < Plan.DirectionalShadow->View.CascadeCount; ++Cascade)
					Admitted[1 + Cascade] = Capture(Plan.DirectionalShadow->StaticMeshes[Cascade]);
		};
		RenderOffscreen(false, false);
		const FMembership Ready = Admitted;
		const auto ReadyDraws = GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws;
		const auto ReadyShadowDraws = GSceneCloudTelemetry.DirectionalShadow.ShadowSuccessfulDraws;
		constexpr uint64 PendingId = 10001;
		auto Expected = Ready;
		ASSERT_GT(std::erase(Expected[0], PendingId), 0u);
		ExtraPolicy.PendingPrimitive = PendingId;
		RenderOffscreen(false, false);
		EXPECT_EQ(Admitted, Expected);
		EXPECT_GT(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, 0u);
		EXPECT_LT(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, ReadyDraws);
		EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowSuccessfulDraws, ReadyShadowDraws);
		ExtraPolicy.PendingPrimitive.reset();
		Expected = Ready;
		uint32 ParticipatingCascades = 0;
		for (uint32 Cascade = 0; Cascade < DirectionalShadowCascadeCount; ++Cascade)
			if (std::ranges::find(Ready[1 + Cascade], PendingId) != Ready[1 + Cascade].end())
			{
				++ParticipatingCascades;
				ExtraPolicy.PendingShadowCascade = Cascade;
			}
		ASSERT_GE(ParticipatingCascades, 2u);
		const uint32 TargetCascade = *ExtraPolicy.PendingShadowCascade;
		const auto Missing = std::erase(Expected[1 + TargetCascade], PendingId);
		ExtraPolicy.PendingShadowPrimitive = PendingId;
		const auto ShadowPreparation = FRenderResourcePreparation::Create();
		auto RenderShadowRequest = [&] {
			RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
				ERenderMode::Lit, ESceneRasterFailure::None, 96, 64, ERasterMode::Solid, ShadowPreparation);
		};
		RenderShadowRequest();
		EXPECT_EQ(ShadowPreparation.GetProgress().State, ERenderResourcePreparationState::Pending);
		EXPECT_EQ(Admitted, Expected);
		EXPECT_EQ(GSceneCloudTelemetry.GBuffer.GBufferStaticMeshSuccessfulDraws, ReadyDraws);
		EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowSuccessfulDraws + Missing, ReadyShadowDraws);
		EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowCascades[TargetCascade].WithheldPrimitives, 1u);

		// The same pending shadow cannot delay capture for a view without shadow demand.
		RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution, ERenderMode::Unlit);
		EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowSceneTraversals, 0u);
		ExtraPolicy.PendingShadowPrimitive.reset();
		RenderShadowRequest();
		EXPECT_EQ(ShadowPreparation.GetProgress().State, ERenderResourcePreparationState::Ready);
		EXPECT_EQ(Admitted, Ready);
		EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowSuccessfulDraws, ReadyShadowDraws);
		ExtraPolicy = {};
		if (GDynamicRHI->RHIGetCapabilities()->bSupportsNonSolidFill)
		{
			RenderOffscreen(false, false, EGroundTruthAmbientOcclusionQuality::HalfResolution,
				ERenderMode::Lit, ESceneRasterFailure::None, 96, 64, ERasterMode::Wireframe);
			EXPECT_TRUE(HasPass("Scene.Forward"));
			EXPECT_GT(GSceneCloudTelemetry.StaticMesh.StaticMeshSuccessfulDraws, 0u);
			EXPECT_EQ(GSceneCloudTelemetry.DirectionalShadow.ShadowSceneTraversals, 1u);
		}

		if (GDynamicRHI->RHIGetCapabilities()->bSupportsIndirectDraw)
		{
			// Leave a hole in the resolved array while a full 64-member group survives.
			constexpr uint64 GroupStart = 12000;
			for (uint64 Index = 0; Index < 65; ++Index)
				FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Scene, FPrimitiveComponentId(GroupStart + Index),
					std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector{LitMaterial->GetRenderProxy()}),
					Math::TranslationMatrix(FVector3{120.0 + Index, double(Index % 8) * 2.0, 2510.0}));
			FlushRenderingCommands();
			ExtraOptions.bEnableGPUCulling = true;
			RenderOffscreen(false, false);
			ExtraPolicy.PendingPrimitive = GroupStart + 32;
			ExtraOptions.bEnableGPUCulling = false;
			const auto Direct = RenderOffscreen(false, false);
			ExtraOptions.bEnableGPUCulling = true;
			EXPECT_EQ(*Direct, *RenderOffscreen(false, false));
			EXPECT_EQ(GSceneCloudTelemetry.StaticMesh.WithheldPrimitives, 1u);
			EXPECT_GT(GSceneCloudTelemetry.GBuffer.GPUCullingIndirectDraws, 0u);
		}

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
		CollectGarbage(Durin::EObjectFlags::Standalone);
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
