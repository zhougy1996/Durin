#include "Actors/ProceduralSkyActor.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Actors/SkyLightActor.h"
#include "Components/ProceduralSkyComponent.h"
#include "Components/SkyLightComponent.h"
#include <glm/gtc/packing.hpp>
#include <thread>
#include "Threading/Task.h"
#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "SkyBoxTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Client/SceneViewport.h"
#include "Modules/ModuleTestSupport.h"
#include "Misc/FileHelper.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneViewState.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Texture/TextureCubeBuilder.h"

namespace
{
	Durin::FViewRenderTelemetry GHybridSkyTelemetry;

	auto CaptureHybridSkyTelemetry(
		const Durin::FViewRenderTelemetry& Telemetry
	) -> void
	{
		GHybridSkyTelemetry = Telemetry;
	}

	auto ProjectPanoramaFixture(
		const std::filesystem::path& Path,
		const Durin::AssetForge::Builtins::FTextureCubePanoramaImportSettings& Settings,
		Durin::FTextureCubeDecodedFaces& OutSource,
		std::string& OutError
	) -> bool
	{
		Durin::FByteBuffer EncodedBytes;
		if (!Durin::FFileHelper::LoadFileToArray(EncodedBytes, Path))
		{
			OutError = "Failed to read the panorama fixture.";
			return false;
		}
		Durin::AssetForge::Builtins::FTextureCubePanoramaSourceData Panorama;
		if (!Durin::AssetForge::Builtins::TranslateTextureCubePanoramaSource(
				EncodedBytes, Path.extension().generic_string(), Panorama, OutError))
			return false;
		return std::visit([&](const auto& Source) {
			return Durin::TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Source, {Settings.FaceDimension, Settings.ExposureEV}, OutSource, OutError);
		}, Panorama);
	}
} // namespace

static void ValidateDynamicSkyLighting(Durin::FRendererModule& Renderer);

TEST(FSkyBoxVulkanTests, SamplesPanoramaFacesMipsBoundariesAndHdrWithoutParallax)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	std::vector<Durin::FMountPoint> MountDefinitions(
		Durin::FMountPaths::GetRegisteredMountPoints().begin(),
		Durin::FMountPaths::GetRegisteredMountPoints().end()
	);
	const std::filesystem::path AssetRoot =
		Durin::Testing::GetTestWorkDirectory() / "SkyBoxAssets";
	Durin::Testing::RemoveTestWorkDirectory(AssetRoot);
	std::filesystem::create_directories(AssetRoot);
	MountDefinitions.push_back({.VirtualRoot = "/SkyBoxAssetTests/", .Owner = Durin::EMountOwner::Test, .Root = AssetRoot, .bAutoScan = true, .bContentWritable = true});
	Durin::Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	ASSERT_TRUE(Durin::InitializeTaskScheduler());
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	Durin::FRendererModule Renderer;
	Durin::FScenePtr SceneOwner = Renderer.CreateScene();
	auto& Scene = static_cast<Durin::FScene&>(*SceneOwner);
	struct FBeginSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginSkyBoxValidationFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
	});
	Durin::FModuleTestHarness RendererLifecycle("SkyBoxRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::FSceneViewStateOwner ViewStateOwner = Renderer.CreateViewState();
	ASSERT_TRUE(ViewStateOwner);
	const Durin::FSceneViewStateId ViewStateId = ViewStateOwner.GetId();
	Durin::FSceneViewStateOwner StaleViewStateOwner = Renderer.CreateViewState();
	ASSERT_TRUE(StaleViewStateOwner);
	const Durin::FSceneViewStateId StaleViewStateId =
		StaleViewStateOwner.GetId();
	StaleViewStateOwner.Reset();
	auto BackloggedViewport = Durin::FSceneViewport::CreateOffscreen(nullptr);
	BackloggedViewport->InitializeViewState(&Renderer);
	std::weak_ptr<Durin::FSceneViewport> BackloggedViewportWeak =
		BackloggedViewport;
	struct FRetainBackloggedSceneViewport
	{
		static constexpr auto GetName() -> const char*
		{
			return "RetainBackloggedSceneViewport";
		}
	};
	Durin::EnqueueRenderCommand<FRetainBackloggedSceneViewport>(
		[BackloggedViewport](Durin::FRHICommandListImmediate&) {});
	BackloggedViewport.reset();

	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> CubeResult = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		GetSkyBoxPanoramaFixture("AnalyticalLDR.tga").generic_string(),
		"/SkyBoxAssetTests/VulkanPanoramaLdr",
		{.FaceDimension = 64}
	);
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	auto CubeReference = CubeResult.Asset->GetTextureReferenceRHI();
	ASSERT_NE(CubeReference, nullptr);
	auto PlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*CubeResult.Asset->GetPlatformData());
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> HdrCubeResult = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		GetSkyBoxPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
		"/SkyBoxAssetTests/VulkanPanoramaHdr",
		{.FaceDimension = 64, .ExposureEV = 1.0f, .Output = Durin::ETextureCubeOutput::HDR}
	);
	ASSERT_TRUE(HdrCubeResult) << HdrCubeResult.Message;
	ASSERT_EQ(HdrCubeResult.Asset->GetBuiltPixelFormat(), Durin::EPixelFormat::RGBA32_FLOAT);
	auto HdrCubeReference = HdrCubeResult.Asset->GetTextureReferenceRHI();
	ASSERT_NE(HdrCubeReference, nullptr);
	auto HdrPlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*HdrCubeResult.Asset->GetPlatformData());
	Durin::FTextureCubeDecodedFaces SourceData;
	std::string ProjectionError;
	ASSERT_TRUE(ProjectPanoramaFixture(
		GetSkyBoxPanoramaFixture("AnalyticalLDR.tga"),
		{.FaceDimension = 64}, SourceData, ProjectionError)) << ProjectionError;
	std::array<std::array<uint8, 4>, Durin::TextureCubeFaceCount> SourceColors;
	std::array<std::array<uint8, 4>, Durin::TextureCubeFaceCount> HdrSourceColors;
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
	{
		SourceColors[FaceIndex] = GetSourceColor(
			SourceData, static_cast<Durin::ETextureCubeFace>(FaceIndex), 32, 32
		);
		std::array<float, 4> Linear;
		std::memcpy(Linear.data(), HdrPlatformData->Faces[FaceIndex].Mips[0].Pixels.data()
			+ (32 * 64 + 32) * 16, 16);
		const auto Mapped = Durin::DisplayMapping::MapSceneLinearToDisplayLinear(
			{Linear[0], Linear[1], Linear[2]}, 0.0f);
		for (uint32 Channel = 0; Channel < 3; ++Channel)
		{
			const float Value = Mapped[Channel];
			const float Encoded = Value <= 0.0031308f ? Value * 12.92f
				: 1.055f * std::pow(Value, 1.0f / 2.4f) - 0.055f;
			HdrSourceColors[FaceIndex][Channel] = static_cast<uint8>(std::lround(std::clamp(Encoded, 0.0f, 1.0f) * 255.0f));
		}
		HdrSourceColors[FaceIndex][3] = 255;
		SourceColors[FaceIndex] =
			MapSrgbReferenceThroughDisplay(SourceColors[FaceIndex]);
	}
	auto* OcclusionMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* OcclusionMaterial = Durin::NewObject<Durin::DMaterial>(nullptr, "SkyBoxOcclusionMaterial");
	const auto OcclusionMaterialValidation = OcclusionMaterial->SetMaterialProgram(
		Durin::MakePBRMaterialProgram());
	ASSERT_TRUE(OcclusionMaterialValidation);
	Durin::FMaterialStaticProperties OcclusionProperties;
	OcclusionProperties.bTwoSided = true;
	ASSERT_TRUE(OcclusionMaterial->SetStaticProperties(OcclusionProperties));
	OcclusionMaterial->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), {1.0, 0.0, 0.0});
	Durin::DObject* OcclusionMaterialCompilationObject = OcclusionMaterial;
	Durin::FAssetCompilingManager::Get().FinishCompilationForObjects(
		std::span<Durin::DObject* const>(&OcclusionMaterialCompilationObject, 1));
	ASSERT_TRUE(OcclusionMaterial->GetMaterialCompileStatus().IsCurrent());
	auto* OcclusionComponent = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SkyBoxOcclusionMesh");
	OcclusionComponent->SetStaticMesh(OcclusionMesh);
	OcclusionComponent->SetMaterial(OcclusionMaterial);
	auto OcclusionProxy = std::make_shared<std::unique_ptr<Durin::FPrimitiveSceneProxy>>(
		OcclusionComponent->CreateSceneProxy()
	);
	ASSERT_NE(*OcclusionProxy, nullptr);

	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.TextureReference = CubeReference;
	Durin::FSkyBoxSceneProxy* SkyBoxToken = PublishSkyBox(Scene, SkyBox);

	struct FEndSkyBoxValidationSetupFrame
	{
		static constexpr auto GetName() -> const char* { return "EndSkyBoxValidationSetupFrame"; }
	};
	Durin::EnqueueRenderCommand<FEndSkyBoxValidationSetupFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
	});
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(BackloggedViewportWeak.expired());

	auto ObservedCubeTarget =
		std::make_shared<std::atomic<Durin::FRHITexture*>>(nullptr);
	struct FObserveInitialCubeTarget
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveInitialCubeTarget";
		}
	};
	Durin::EnqueueRenderCommand<FObserveInitialCubeTarget>(
		[CubeReference, ObservedCubeTarget](
			Durin::FRHICommandListImmediate&
		) {
			ObservedCubeTarget->store(
				CubeReference->GetReferencedTexture_RenderThread(),
				std::memory_order_release
			);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FRHITexture* InitialCubeTarget =
		ObservedCubeTarget->load(std::memory_order_acquire);
	ASSERT_NE(InitialCubeTarget, nullptr);
	Durin::PumpGameThreadDeferredWork();
	ASSERT_FALSE(CubeResult.Asset->IsResourceUpdatePending());

	ASSERT_TRUE(CubeResult.Asset->RebuildPlatformData());
	EXPECT_EQ(CubeResult.Asset->GetTextureReferenceRHI(), CubeReference);
	Durin::FlushRenderingCommands();
	Durin::PumpGameThreadDeferredWork();
	EXPECT_TRUE(CubeResult.Asset->HasUsableResource());
	EXPECT_FALSE(CubeResult.Asset->IsResourceUpdatePending());
	struct FObserveReplacementCubeTarget
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveReplacementCubeTarget";
		}
	};
	Durin::EnqueueRenderCommand<FObserveReplacementCubeTarget>(
		[CubeReference, ObservedCubeTarget](
			Durin::FRHICommandListImmediate&
		) {
			ObservedCubeTarget->store(
				CubeReference->GetReferencedTexture_RenderThread(),
				std::memory_order_release
			);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FRHITexture* ReplacementCubeTarget =
		ObservedCubeTarget->load(std::memory_order_acquire);
	ASSERT_NE(ReplacementCubeTarget, nullptr);
	EXPECT_NE(ReplacementCubeTarget, InitialCubeTarget);

	struct FValidationResult
	{
		bool bSucceeded = true;
		std::string Error;
		std::array<Durin::FByteBuffer, Durin::TextureCubeFaceCount> PrincipalAxes;
		std::array<Durin::FByteBuffer, Durin::TextureCubeFaceCount> HdrPrincipalAxes;
		std::array<Durin::FByteBuffer, 2> LongitudeSeam;
		std::array<Durin::FByteBuffer, 2> FaceBoundary;
		Durin::FByteBuffer Translated;
		Durin::FByteBuffer StatefulNoConsumer;
		Durin::FByteBuffer StaleStateNoConsumer;
		Durin::FByteBuffer StatefulLitNoConsumer;
		Durin::FByteBuffer StatefulLetterboxedNoConsumer;
		Durin::FByteBuffer ComponentRotated;
		Durin::FByteBuffer ExplicitOverride;
		Durin::FByteBuffer Letterboxed;
		Durin::FByteBuffer Occluded;
		Durin::FByteBuffer ForwardLitSky;
		Durin::FByteBuffer HybridLitSky;
		Durin::FViewRenderTelemetry HybridSkyTelemetry;
		Durin::ERenderViewResult InvalidOutputResult =
			Durin::ERenderViewResult::Success;
		Durin::ERenderViewResult MissingEnvironmentResult =
			Durin::ERenderViewResult::Success;
		Durin::FSceneViewStatistics FirstViewStatistics;
		Durin::FSceneViewStatistics InvalidViewStatistics;
		bool bCapturedFirstViewStatistics = false;
		bool bTypedHistoryCandidateCommitted = false;
		bool bTypedHistoryAbortRetainedCommitted = false;
		bool bTypedHistoryResetReleased = false;
	};
	auto Result = std::make_shared<FValidationResult>();

	struct FRenderSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "RenderSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FRenderSkyBoxValidationFrame>(
		[&Renderer, &Scene, CubeReference, PlatformData, ViewStateId,
			StaleViewStateId,
		 HdrCubeReference, HdrPlatformData, Result, OcclusionProxy,
		 SkyBoxToken](Durin::FRHICommandListImmediate& CommandList) mutable {
			Durin::GRenderFrameCounterRenderThread++;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			struct FEndFrameGuard
			{
				Durin::FRHICommandListImmediate& CommandList;
				~FEndFrameGuard() { Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList); }
			} EndFrameGuard{CommandList};

			Durin::FRHITexture* CubeTexture =
				CubeReference->GetReferencedTexture_RenderThread();
			if (CubeTexture == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Cube render resource was not ready.";
				return;
			}
			for (uint32 FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
			{
				for (uint32 MipIndex = 0; MipIndex < PlatformData->Faces[FaceIndex].Mips.size(); ++MipIndex)
				{
					Durin::FByteBuffer MipPixels;
					if (!Durin::GDynamicRHI->RHIReadTexture2D(
							CommandList, CubeTexture, MipIndex, FaceIndex, MipPixels
						)
						|| MipPixels != PlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels)
					{
						Result->bSucceeded = false;
						Result->Error = std::format(
							"Cube readback mismatch for face {} mip {}.", FaceIndex, MipIndex
						);
						return;
					}
				}
			}
			Durin::FRHITexture* HdrCubeTexture =
				HdrCubeReference->GetReferencedTexture_RenderThread();
			if (HdrCubeTexture == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "HDR cube render resource was not ready.";
				return;
			}
			for (uint32 FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
			{
				for (uint32 MipIndex = 0; MipIndex < HdrPlatformData->Faces[FaceIndex].Mips.size(); ++MipIndex)
				{
					Durin::FByteBuffer MipPixels;
					if (!Durin::GDynamicRHI->RHIReadTexture2D(
							CommandList, HdrCubeTexture, MipIndex, FaceIndex, MipPixels
						)
						|| MipPixels != HdrPlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels)
					{
						Result->bSucceeded = false;
						Result->Error = std::format(
							"HDR-derived cube readback mismatch for face {} mip {}.", FaceIndex, MipIndex
						);
						return;
					}
				}
			}

			Durin::FRHITextureCreateDesc ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
														 "SkyBoxValidationColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM
			)
														 .SetFlags(Durin::ETextureCreateFlags::RenderTargetable | Durin::ETextureCreateFlags::ShaderResource | Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Color = Durin::GDynamicRHI->RHICreateTexture(CommandList, ColorDesc);
			if (Color == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Failed to create the validation output target.";
				return;
			}
			Durin::FSceneViewHistoryProbe HistoryProbe;
			HistoryProbe.PendingRevision = 1;
			HistoryProbe.PendingTexture = Color;
			HistoryProbe.Commit();
			Result->bTypedHistoryCandidateCommitted =
				HistoryProbe.CommittedRevision == 1
				&& HistoryProbe.CommittedTexture == Color;
			HistoryProbe.PendingRevision = 2;
			HistoryProbe.PendingTexture = Color;
			HistoryProbe.Abort();
			Result->bTypedHistoryAbortRetainedCommitted =
				HistoryProbe.CommittedRevision == 1
				&& HistoryProbe.CommittedTexture == Color
				&& HistoryProbe.PendingTexture == nullptr;
			HistoryProbe.Reset();
			Result->bTypedHistoryResetReleased =
				HistoryProbe.CommittedRevision == 0
				&& HistoryProbe.CommittedTexture == nullptr;

			auto RenderWithOptions = [&](const Durin::FSceneView& View,
										 Durin::FByteBuffer& OutPixels,
										 const Durin::FSceneViewRenderOptions& Options,
										 Durin::ERenderMode RenderMode = Durin::ERenderMode::Unlit) {
				Durin::FSceneView RenderView = View;
				RenderView.Settings.Mode.RenderMode = RenderMode;
				Durin::FSceneViewStatistics Statistics;
				if (Renderer.RenderView(
						CommandList, &Scene, RenderView, Color, false, Options,
						&Statistics
					)
					!= Durin::ERenderViewResult::Success)
				{
					Result->bSucceeded = false;
					Result->Error = "The validation view did not render successfully.";
					return false;
				}
				if (!Result->bCapturedFirstViewStatistics)
				{
					Result->FirstViewStatistics = Statistics;
					Result->bCapturedFirstViewStatistics = true;
				}
				if (!Durin::GDynamicRHI->RHIReadTexture2D(CommandList, Color, 0, 0, OutPixels))
				{
					Result->bSucceeded = false;
					Result->Error = "Failed to read the validation render target.";
					return false;
				}
				return true;
			};
			auto Render = [&](const Durin::FSceneView& View,
							  Durin::FByteBuffer& OutPixels) {
				return RenderWithOptions(View, OutPixels, {});
			};

			constexpr std::array<Durin::FVector3, Durin::TextureCubeFaceCount> Directions = {
				Durin::FVector3(1.0, 0.0, 0.0),
				Durin::FVector3(-1.0, 0.0, 0.0),
				Durin::FVector3(0.0, 1.0, 0.0),
				Durin::FVector3(0.0, -1.0, 0.0),
				Durin::FVector3(0.0, 0.0, 1.0),
				Durin::FVector3(0.0, 0.0, -1.0)
			};
			for (size_t FaceIndex = 0; FaceIndex < Directions.size(); ++FaceIndex)
			{
				if (!Render(MakePrincipalAxisView(Directions[FaceIndex], {}, 17, 17), Result->PrincipalAxes[FaceIndex])) return;
			}
			Durin::FSceneView StatefulView =
				MakePrincipalAxisView(Directions[0], {}, 17, 17);
			StatefulView.ViewStateId = ViewStateId;
			if (!Render(StatefulView, Result->StatefulNoConsumer)) return;
			Durin::FSceneView StaleStateView =
				MakePrincipalAxisView(Directions[0], {}, 17, 17);
			StaleStateView.ViewStateId = StaleViewStateId;
			if (!Render(StaleStateView, Result->StaleStateNoConsumer)) return;
			constexpr double EdgeOffset = 0.02;
			if (!Render(MakePrincipalAxisView({-1.0, EdgeOffset, 0.0}, {}, 17, 17), Result->LongitudeSeam[0])) return;
			if (!Render(MakePrincipalAxisView({-1.0, -EdgeOffset, 0.0}, {}, 17, 17), Result->LongitudeSeam[1])) return;
			if (!Render(MakePrincipalAxisView({1.0, 1.0 - EdgeOffset, 0.0}, {}, 17, 17), Result->FaceBoundary[0])) return;
			if (!Render(MakePrincipalAxisView({1.0 - EdgeOffset, 1.0, 0.0}, {}, 17, 17), Result->FaceBoundary[1])) return;
			if (!Render(MakePrincipalAxisView(Directions[0], {19.0, -7.0, 4.0}, 17, 17), Result->Translated)) return;

			Durin::FSkyBoxSceneData RotatedSky;
			RotatedSky.TextureReference = CubeReference;
			RotatedSky.Rotation = glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up);
			Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, SkyBoxToken);
			SkyBoxToken = PublishSkyBox(Scene, RotatedSky);
			if (!Render(MakePrincipalAxisView(Directions[0], {}, 17, 17), Result->ComponentRotated)) return;

			RotatedSky.TextureReference = HdrCubeReference;
			RotatedSky.Rotation = glm::identity<Durin::FQuat>();
			Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, SkyBoxToken);
			SkyBoxToken = PublishSkyBox(Scene, RotatedSky);
			Durin::FSceneViewRenderOptions OverrideOptions;
			OverrideOptions.Environment = Durin::FViewEnvironmentOverride{
				.TextureReference = CubeReference
			};
			if (!RenderWithOptions(
					MakePrincipalAxisView(Directions[0], {}, 17, 17),
					Result->ExplicitOverride,
					OverrideOptions
				)) return;
			Result->InvalidViewStatistics.Summary.Triangles = 999;
			Result->InvalidViewStatistics.Summary.DrawCalls = 999;
			Result->InvalidOutputResult = Renderer.RenderView(
				CommandList,
				&Scene,
				MakePrincipalAxisView(Directions[0], {}, 17, 17),
				nullptr,
				false,
				{},
				&Result->InvalidViewStatistics
			);
			Durin::FSceneViewRenderOptions MissingEnvironment;
			MissingEnvironment.Environment = Durin::FViewEnvironmentOverride{};
			Result->MissingEnvironmentResult = Renderer.RenderView(
				CommandList,
				&Scene,
				MakePrincipalAxisView(Directions[0], {}, 17, 17),
				Color,
				false,
				MissingEnvironment
			);
			for (size_t FaceIndex = 0; FaceIndex < Directions.size(); ++FaceIndex)
			{
				if (!Render(MakePrincipalAxisView(Directions[FaceIndex], {}, 17, 17), Result->HdrPrincipalAxes[FaceIndex])) return;
			}

			RotatedSky.TextureReference = CubeReference;
			Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, SkyBoxToken);
			SkyBoxToken = PublishSkyBox(Scene, RotatedSky);
			Durin::FSceneView LetterboxView = MakePrincipalAxisView(Directions[0], {}, 17, 17);
			LetterboxView.AspectRatioConstraint = 0.5f;
			if (!Render(LetterboxView, Result->Letterboxed)) return;
			LetterboxView.ViewStateId = ViewStateId;
			if (!Render(
					LetterboxView,
					Result->StatefulLetterboxedNoConsumer)) return;

			RotatedSky.Rotation = glm::identity<Durin::FQuat>();
			Durin::FSceneInterfaceTestAccess::TryRemoveSkyBoxProxy(Scene, SkyBoxToken);
			SkyBoxToken = PublishSkyBox(Scene, RotatedSky);
			const Durin::FSceneView HybridSkyView =
				MakePrincipalAxisView(Directions[4], {}, 17, 17);
			if (!RenderWithOptions(HybridSkyView, Result->ForwardLitSky, {}, Durin::ERenderMode::Lit)) return;
			Durin::FSceneView StatefulHybridSkyView = HybridSkyView;
			StatefulHybridSkyView.ViewStateId = ViewStateId;
			if (!RenderWithOptions(
					StatefulHybridSkyView, Result->StatefulLitNoConsumer,
					{}, Durin::ERenderMode::Lit)) return;
			Durin::FSceneViewRenderOptions HybridSkyOptions;
			Durin::SetViewRenderTelemetrySink(CaptureHybridSkyTelemetry);
			const bool bRenderedHybridSky = RenderWithOptions(HybridSkyView, Result->HybridLitSky, HybridSkyOptions, Durin::ERenderMode::Lit);
			Result->HybridSkyTelemetry = GHybridSkyTelemetry;
			Durin::SetViewRenderTelemetrySink(nullptr);
			if (!bRenderedHybridSky) return;
			Durin::FMatrix OccluderTransform = glm::translate(
				Durin::FMatrix(1.0), Durin::FVector3(0.0, 0.0, 0.5)
			);
			OccluderTransform = glm::rotate(
				OccluderTransform, glm::pi<double>(), Durin::FVectorConstants::Right
			);
			Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(1), std::move(*OcclusionProxy), OccluderTransform);
			Render(MakePrincipalAxisView(Directions[4], {}, 17, 17), Result->Occluded);
		}
	);
	Durin::FlushRenderingCommands();
	ViewStateOwner.Reset();
	Durin::FlushRenderingCommands();

	EXPECT_TRUE(Result->bSucceeded) << Result->Error;
	if (Result->bSucceeded)
	{
		EXPECT_EQ(
			Result->InvalidOutputResult,
			Durin::ERenderViewResult::InvalidOutput
		);
		EXPECT_EQ(
			Result->MissingEnvironmentResult,
			Durin::ERenderViewResult::RequiredEnvironmentUnavailable
		);
		EXPECT_TRUE(Result->bCapturedFirstViewStatistics);
		EXPECT_TRUE(Result->bTypedHistoryCandidateCommitted);
		EXPECT_TRUE(Result->bTypedHistoryAbortRetainedCommitted);
		EXPECT_TRUE(Result->bTypedHistoryResetReleased);
		EXPECT_EQ(Result->FirstViewStatistics.Visibility.VisiblePrimitives, 0u);
		EXPECT_EQ(Result->FirstViewStatistics.Summary.Triangles, 0u);
		EXPECT_GT(Result->FirstViewStatistics.Summary.DrawCalls, 0u);
		EXPECT_EQ(Result->InvalidViewStatistics, Durin::FSceneViewStatistics{});
		EXPECT_EQ(Result->ExplicitOverride, Result->PrincipalAxes[0]);
		EXPECT_EQ(Result->StatefulNoConsumer, Result->PrincipalAxes[0]);
		EXPECT_EQ(Result->StaleStateNoConsumer, Result->PrincipalAxes[0]);
		EXPECT_EQ(Result->StatefulLitNoConsumer, Result->ForwardLitSky);
		EXPECT_EQ(
			Result->StatefulLetterboxedNoConsumer,
			Result->Letterboxed);
		EXPECT_EQ(Result->HybridLitSky, Result->ForwardLitSky);
		EXPECT_EQ(Result->HybridSkyTelemetry.Deferred.HybridDeferredEnabledViews, 1u);
		EXPECT_EQ(Result->HybridSkyTelemetry.Deferred.HybridDeferredUnavailableViews, 0u);
		for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		{
			SCOPED_TRACE(std::format("principal face {}", FaceIndex));
			ExpectRgbNear(
				Result->PrincipalAxes[FaceIndex],
				17,
				8,
				8,
				SourceColors[FaceIndex],
				12
			);
			ExpectRgbNear(
				Result->HdrPrincipalAxes[FaceIndex],
				17,
				8,
				8,
				HdrSourceColors[FaceIndex],
				12
			);
		}
		ExpectRgbMatch(Result->LongitudeSeam[0], Result->LongitudeSeam[1], 17, 8, 8, 12);
		ExpectRgbMatch(Result->FaceBoundary[0], Result->FaceBoundary[1], 17, 8, 8, 16);
		ExpectRgbMatch(Result->Translated, Result->PrincipalAxes[0], 17, 8, 8);
		EXPECT_EQ(FindClosestCenterRgb(Result->ComponentRotated, Result->PrincipalAxes, 17), 3u);
		ExpectRgbNear(Result->Letterboxed, 17, 1, 8, {0, 0, 0, 255}, 2);
		EXPECT_EQ(FindClosestCenterRgb(Result->Letterboxed, Result->PrincipalAxes, 17), 0u);
		ExpectRgbNear(Result->Occluded, 17, 8, 8, MapSrgbReferenceThroughDisplay({255, 0, 0, 255}), 8);
	}

	ValidateDynamicSkyLighting(Renderer);
	Durin::FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
	Durin::FlushRenderingCommands();
	SkyBox.TextureReference = nullptr;
	Durin::FPackagePath CubePath;
	if (Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/VulkanPanoramaLdr", CubePath))
	{
		EXPECT_TRUE(Durin::UnloadPackage(
			CubePath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
		const Durin::FAssetResult DeleteResult = Durin::Testing::RemoveAssetPackageForTests(CubePath);
		EXPECT_TRUE(DeleteResult) << DeleteResult.Message;
	}
	else
	{
		ADD_FAILURE() << "Failed to create the Vulkan cube cleanup path.";
	}
	Durin::FPackagePath HdrCubePath;
	if (Durin::FPackagePath::TryCreate("/SkyBoxAssetTests/VulkanPanoramaHdr", HdrCubePath))
	{
		EXPECT_TRUE(Durin::UnloadPackage(
			HdrCubePath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
		const Durin::FAssetResult DeleteResult = Durin::Testing::RemoveAssetPackageForTests(HdrCubePath);
		EXPECT_TRUE(DeleteResult) << DeleteResult.Message;
	}
	else
	{
		ADD_FAILURE() << "Failed to create the Vulkan HDR cube cleanup path.";
	}
	Durin::MarkAsGarbage(OcclusionComponent);
	Durin::MarkAsGarbage(OcclusionMesh);
	Durin::MarkAsGarbage(OcclusionMaterial);
	Durin::CollectGarbage();
	struct FRetireSkyBoxValidationResource
	{
		static constexpr auto GetName() -> const char* { return "RetireSkyBoxValidationResource"; }
	};
	Durin::EnqueueRenderCommand<FRetireSkyBoxValidationResource>(
		[Reference = std::move(CubeReference),
		 HdrReference = std::move(HdrCubeReference)](
			Durin::FRHICommandListImmediate&
		) {}
	);
	Durin::FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::ShutdownTaskSystem();
	Durin::RHIExit();
	Durin::ShutdownAssetCompilingManager();
}

static void ValidateDynamicSkyLighting(Durin::FRendererModule& Renderer)
{
    using namespace Durin;
    TryEnqueueRenderCommand("BeginSkySetup",[](FRHICommandListImmediate& Cmd) { GDynamicRHI->RHIBeginFrame_RenderThread(Cmd); });
    auto SceneOwner=Renderer.CreateScene();
    auto& Scene=static_cast<FScene&>(*SceneOwner);
    auto* World=NewObject<DWorld>(nullptr,"DynamicSkyWorld");
    AddToRoot(World);
    EXPECT_TRUE(World->InitializeSubsystems());
    World->SetRenderScene(&Scene);
    EXPECT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World,"SkyLevel")));
    auto* Sky=World->SpawnActor<AProceduralSkyActor>("Sky")->GetSkyComponent();
    auto* Light=World->SpawnActor<ASkyLightActor>("Light")->GetSkyLightComponent();
    auto OtherSceneOwner=Renderer.CreateScene();
    auto& OtherScene=static_cast<FScene&>(*OtherSceneOwner);
    auto* OtherWorld=NewObject<DWorld>(nullptr,"OtherSkyWorld");
    AddToRoot(OtherWorld); EXPECT_TRUE(OtherWorld->InitializeSubsystems());
    OtherWorld->SetRenderScene(&OtherScene);
    EXPECT_TRUE(OtherWorld->SetCurrentLevel(NewObject<DLevel>(OtherWorld,"OtherSkyLevel")));
    OtherWorld->SpawnActor<AProceduralSkyActor>("OtherSky")->GetSkyComponent()->SetRadianceColors({1,3,5},{1,3,5},{1,3,5},{0,0,0});
    auto* OtherLight=OtherWorld->SpawnActor<ASkyLightActor>("OtherLight")->GetSkyLightComponent();
    OtherLight->SetSource(ESkyLightSourceMode::CapturedSky,nullptr);
    OtherLight->SetRefreshPolicy(true,0.25f);
    Sky->SetExposure(4);
    Light->SetSource(ESkyLightSourceMode::CapturedSky,nullptr);
    Light->SetRefreshPolicy(true,0.25f);
    TryEnqueueRenderCommand("EndSkySetup",[](FRHICommandListImmediate& Cmd) { GDynamicRHI->RHIEndFrame_RenderThread(Cmd); });
    FlushRenderingCommands();
    bool Ready=false;
    uint64 Revision=0;
    FByteBuffer Before,After;
    const auto Pump=[&](bool Read) {
        TryEnqueueRenderCommand("BeginSkyTick",[](FRHICommandListImmediate& Cmd) { ++GRenderFrameCounterRenderThread; GDynamicRHI->RHIBeginFrame_RenderThread(Cmd); });
        World->Tick({.DeltaSeconds=0.02f});
        OtherWorld->Tick({.DeltaSeconds=0.02f});
        TryEnqueueRenderCommand("EndSkyTick",[&](FRHICommandListImmediate& Cmd) {
            Renderer.UpdateScenes_RenderThread(Cmd);
            Ready=bool(Scene.SkyLighting->Active);
            if (Ready)
            {
                const auto& G=*Scene.SkyLighting->Active;
                Revision=G.Revision;
                EXPECT_EQ(G.Radiance->GetNumMips(),8u);
                EXPECT_EQ(G.Prefiltered->GetNumMips(),8u);
                EXPECT_EQ(G.Irradiance->GetSizeX(),16u);
                if(Read) EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Radiance,0,4,After));
            }
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
    };
    for(int i=0;i<150 && !Ready;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    EXPECT_TRUE(Ready) << "Scene tick must generate GPU lighting without any view";
    if(Ready)
    {
        Pump(true);
        Before=After;
        const uint64 FirstRevision=Revision;
        const auto Half=[&](const FByteBuffer& Bytes,size_t Index) {
            if (Bytes.size() < Index*2+2) return 0.0f;
            uint16 Bits; std::memcpy(&Bits,Bytes.data()+Index*2,2);
            return glm::unpackHalf2x16(uint32(Bits)).x;
        };
        EXPECT_GT(Half(Before,(64*128+64)*4),1.0f);
        Sky->SetSunDirection(FVector3f(0,0,1));
        for(int i=0;i<100 && Revision==FirstRevision;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_GT(Revision,FirstRevision);
        Pump(true);
        EXPECT_NE(Before,After);
        Sky->SetExposure(0);
        Sky->SetRadianceColors({0,0,0},{0,0,0},{0,0,0},{16,8,4});
        const uint64 HotspotRevision=Revision;
        for(int i=0;i<100 && Revision==HotspotRevision;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_GT(Revision,HotspotRevision);
        TryEnqueueRenderCommand("ValidateSkyHotspot",[&](FRHICommandListImmediate& Cmd) {
            GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            const auto& G=*Scene.SkyLighting->Active;
            FByteBuffer Pixels;
            EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Irradiance,0,4,Pixels));
            // Analytic hemisphere integral of 16*cos(theta)^16*cos(theta).
            EXPECT_NEAR(Half(Pixels,(8*16+8)*4),16.f*2.f*3.14159265f/18.f,0.20f);
            std::array<float,8> Peaks{};
            for(uint32 Mip=0;Mip<8;++Mip)
            {
                EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Prefiltered,Mip,4,Pixels));
                for(size_t Pixel=0;Pixel<Pixels.size()/8;++Pixel) Peaks[Mip]=std::max(Peaks[Mip],Half(Pixels,Pixel*4));
            }
            EXPECT_GT(Peaks[0],15.f);
            EXPECT_GT(Peaks[0],Peaks[2]);
            EXPECT_GT(Peaks[2],Peaks[4]);
            EXPECT_GT(Peaks[4],Peaks[7]);
            EXPECT_LT(Peaks[7],3.f);
            // Visible sky uses the same radiance, then display mapping.
            auto Color=GDynamicRHI->RHICreateTexture(Cmd,FRHITextureCreateDesc::Create2D("VisibleAnalyticSky",17,17,EPixelFormat::SRGBA8_UNORM)
                .SetFlags(ETextureCreateFlags::RenderTargetable|ETextureCreateFlags::ShaderResource|ETextureCreateFlags::CPUReadback));
            EXPECT_EQ(Renderer.RenderView(Cmd,&Scene,MakePrincipalAxisView({0,0,1},{},17,17),Color,false,{}),ERenderViewResult::Success);
            EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,Color,0,0,Pixels));
            const auto Mapped=DisplayMapping::MapSceneLinearToDisplayLinear({16,8,4},0);
            std::array<uint8,4> Expected{0,0,0,255};
            for(uint32 C=0;C<3;++C) Expected[C]=uint8(std::lround((1.055f*std::pow(Mapped[C],1.f/2.4f)-0.055f)*255));
            ExpectRgbNear(Pixels,17,8,8,Expected,4);
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
        Sky->SetRadianceColors({2,4,8},{2,4,8},{2,4,8},{0,0,0});
        const uint64 PreviousRevision=Revision;
        for(int i=0;i<100 && Revision==PreviousRevision;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_GT(Revision,PreviousRevision);
        TryEnqueueRenderCommand("ValidateSkyEnergy",[&](FRHICommandListImmediate& Cmd) {
            GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            EXPECT_TRUE(OtherScene.SkyLighting->Active);
            if (OtherScene.SkyLighting->Active)
            {
                FByteBuffer OtherPixels;
                EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,OtherScene.SkyLighting->Active->Irradiance,0,4,OtherPixels));
                EXPECT_NEAR(Half(OtherPixels,(8*16+8)*4),3.14159265f,0.01f);
                EXPECT_NE(OtherScene.SkyLighting->Active->Owner,Scene.SkyLighting->Active->Owner);
            }
            const auto& G=*Scene.SkyLighting->Active;
            for(uint32 Face=0;Face<6;++Face)
            {
                FByteBuffer Pixels;
                EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Irradiance,0,Face,Pixels));
                for(uint32 Channel=0;Channel<3;++Channel)
                    EXPECT_NEAR(Half(Pixels,(8*16+8)*4+Channel),3.14159265f*float(2u<<Channel),0.025f);
                for(uint32 Mip=0;Mip<8;++Mip)
                {
                    EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Prefiltered,Mip,Face,Pixels));
                    for(uint32 Channel=0;Channel<3;++Channel)
                        EXPECT_NEAR(Half(Pixels,Channel),float(2u<<Channel),0.01f);
                }
            }
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
        // Retain old generations deliberately until allocation admission fails.
        // Failure must leave the last complete active set available, then recover
        // when the outstanding consumers release their references.
        std::vector<std::shared_ptr<const FSkyLightingGeneration>> Retained;
        bool Backpressured=false;
        for(int Attempt=0;Attempt<8 && !Backpressured;++Attempt)
        {
            TryEnqueueRenderCommand("RetainSkyConsumer",[&](FRHICommandListImmediate&) { Retained.push_back(Scene.SkyLighting->Active); });
            FlushRenderingCommands();
            Light->Recapture();
            std::this_thread::sleep_for(std::chrono::milliseconds(270));
            Pump(false);
            Backpressured=Light->GetUpdateStatus()->State.load()==ESkyLightUpdateState::Backpressure;
        }
        EXPECT_TRUE(Backpressured);
        TryEnqueueRenderCommand("CheckLastGoodSky",[&](FRHICommandListImmediate&) {
            EXPECT_EQ(Scene.SkyLighting->Active,Retained.back());
            Retained.clear();
        });
        FlushRenderingCommands();
        for(int i=0;i<100 && Light->GetUpdateStatus()->State.load()!=ESkyLightUpdateState::Ready;++i)
        { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_EQ(Light->GetUpdateStatus()->State.load(),ESkyLightUpdateState::Ready);
        // Manual refresh is independent of the automatic policy. Continuous
        // animation must not cancel a generation already admitted to the GPU.
        Light->SetRefreshPolicy(false,0.25f);
        const uint64 StableRevision=Revision;
        Sky->SetSunDirection({1,0,0});
        Pump(false);
        EXPECT_EQ(Revision,StableRevision);
        Light->Recapture();
        for(int i=0;i<100 && Revision==StableRevision;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_GT(Revision,StableRevision);
        Pump(false);
        TryEnqueueRenderCommand("ReportSkyTiming",[&](FRHICommandListImmediate&) {
            const auto Selected=Scene.GetSkyLight_RenderThread();
            std::cout << "Dynamic sky GPU update: " << Selected->UpdateStatus->UpdateMilliseconds.load() << " ms\n";
        });
        FlushRenderingCommands();
        // Invalidate while the scene is inactive: old ownership must disappear
        // without waiting for another world tick or view submission.
        EXPECT_TRUE(Renderer.RequestResourceInvalidation(ERendererResourceInvalidationCause::Device).bSuccess);
        TryEnqueueRenderCommand("CheckSkyInvalidation",[&](FRHICommandListImmediate&) {
            EXPECT_FALSE(Scene.SkyLighting->Active);
            EXPECT_FALSE(Scene.SkyLighting->InFlight);
            EXPECT_FALSE(OtherScene.SkyLighting->Active);
        });
        FlushRenderingCommands();
        Ready=false;
        for(int i=0;i<150 && !Ready;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_TRUE(Ready);
        // Rapid replacement cannot reuse a previous authority's generation.
        for(int i=0;i<12;++i)
        {
            Light->SetSource(ESkyLightSourceMode::SpecifiedCube,nullptr);
            Pump(false); EXPECT_FALSE(Ready);
            Light->SetSource(ESkyLightSourceMode::CapturedSky,nullptr);
            Pump(false);
        }
        Sky->SetEnabled(false); Pump(false); EXPECT_FALSE(Ready);
        Sky->SetEnabled(true);
        Light->SetEnabled(false);
        Pump(false);
        EXPECT_FALSE(Ready);
    }
    // Detach the second world directly after admitting a manual refresh.
    OtherLight->Recapture(); Pump(false);
    EXPECT_TRUE(OtherWorld->SetCurrentLevel(nullptr)); OtherWorld->SetRenderScene(nullptr);
    OtherWorld->Shutdown(); RemoveFromRoot(OtherWorld); MarkObjectHierarchyAsGarbage(OtherWorld);
    FSceneInterfaceTestAccess::ReleaseScene(OtherSceneOwner);
    EXPECT_TRUE(World->SetCurrentLevel(nullptr));
    World->SetRenderScene(nullptr);
    World->Shutdown();
    RemoveFromRoot(World);
    MarkObjectHierarchyAsGarbage(World);
    CollectGarbage();
    FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
    FlushRenderingCommands();
}
