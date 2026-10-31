#include "NativeAssetBuildTestSupport.h"
#include "RenderResource.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "../Materials/ExplicitMaterialProgramTestFixture.h"
#include "Actors/ProceduralSkyActor.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Actors/SkyLightActor.h"
#include "Components/ProceduralSkyComponent.h"
#include "Components/SkyLightComponent.h"
#include <glm/gtc/packing.hpp>
#include "Threading/Task.h"
#include "RenderResourcePreparation.h"
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

	class FSkyValidationFrame final
	{
	public:
		explicit FSkyValidationFrame(Durin::FRHICommandListImmediate& InCommands) : Commands(InCommands)
		{
			++Durin::GRenderFrameCounterRenderThread;
			Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
		}
		~FSkyValidationFrame()
		{
			Durin::SetViewRenderTelemetrySink(nullptr);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(Commands);
		}
	private:
		Durin::FRHICommandListImmediate& Commands;
	};

	// The host pumps loading frames; render commands never wait for shader compilation.
	auto RenderPreparedSkyView(Durin::FRendererModule& Renderer, Durin::FScene& Scene,
		const Durin::FSceneView& View, const Durin::FTextureRHIRef& Color,
		const Durin::FSceneViewRenderOptions& Options, Durin::FSceneViewStatistics* Statistics,
		const std::function<void(Durin::FRHICommandListImmediate&)>& AfterDraw,
		bool bCaptureTelemetry = false) -> Durin::ERenderViewResult
	{
		using namespace Durin;
		const auto Preparation = FRenderResourcePreparation::Create();
		auto Result = ERenderViewResult::RendererResourcesPending;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		do
		{
			EnqueueRenderCommand("PrepareSkyValidationView", [&](FRHICommandListImmediate& Commands) {
				FSkyValidationFrame Frame(Commands);
				const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
				Renderer.PrepareViewResources(Commands, {&Scene, View, Color, false, Options}, Preparation);
				EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
				const auto State = Preparation.GetProgress().State;
				if (State == ERenderResourcePreparationState::Pending) return;
				if (State != ERenderResourcePreparationState::Ready)
				{ Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				if (bCaptureTelemetry) SetViewRenderTelemetrySink(CaptureHybridSkyTelemetry);
				Result = Renderer.RenderView(Commands, {&Scene, View, Color, false, Options, Statistics});
				if (Result == ERenderViewResult::Success) AfterDraw(Commands);
			});
			FlushRenderingCommands();
			if (Result != ERenderViewResult::RendererResourcesPending || testing::Test::HasFatalFailure()) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		if (Result == ERenderViewResult::RendererResourcesPending)
		{
			Preparation.RequestCancel();
			EnqueueRenderCommand("RetireSkyValidationPreparation", [&](FRHICommandListImmediate&) {
				Preparation.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
			});
			FlushRenderingCommands();
		}
		return Result;
	}

	auto ProjectPanoramaFixture(
		const std::filesystem::path& Path,
		const Durin::AssetForge::Builtins::FTextureCubePanoramaImportSettings& Settings,
		Durin::FTextureCubeFaceImages& OutSource,
		std::string& OutError
	) -> bool
	{
		auto EncodedBytes = Durin::FFileHelper::LoadFileToArray(Path);
		if (!EncodedBytes)
		{
			OutError = "Failed to read the panorama fixture.";
			return false;
		}
		Durin::AssetForge::Builtins::FTextureCubePanoramaSourceData Panorama;
		if (!Durin::AssetForge::Builtins::TranslateTextureCubePanoramaSource(
				*EncodedBytes, Path.extension().generic_string(), Panorama, OutError))
			return false;
		return std::visit([&](const auto& Source) {
			auto Result = Durin::TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Source, {Settings.FaceDimension, Settings.ExposureEV}, OutSource);
			if (!Result) OutError = Result.error().Diagnostic;
			return Result.has_value();
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
	Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
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
	ASSERT_EQ(HdrCubeResult.Asset->GetPixelFormat(), Durin::EPixelFormat::RGBA32_FLOAT);
	auto HdrCubeReference = HdrCubeResult.Asset->GetTextureReferenceRHI();
	ASSERT_NE(HdrCubeReference, nullptr);
	auto HdrPlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*HdrCubeResult.Asset->GetPlatformData());
	Durin::FTextureCubeFaceImages SourceData;
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
	const auto OcclusionMaterialValidation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*OcclusionMaterial);
	ASSERT_TRUE(OcclusionMaterialValidation);
	Durin::FMaterialStaticProperties OcclusionProperties;
	OcclusionProperties.bTwoSided = true;
	ASSERT_TRUE(OcclusionMaterial->SetStaticProperties(OcclusionProperties));
	OcclusionMaterial->SetVectorParameterValue(Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), {1.0, 0.0, 0.0});
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
	ASSERT_FALSE(CubeResult.Asset->HasPendingRenderResourceInitialization());

	CubeResult.Asset->PostEditChange();
	ASSERT_TRUE(CubeResult.Asset->FinishCachePlatformData());
	EXPECT_EQ(CubeResult.Asset->GetTextureReferenceRHI(), CubeReference);
	Durin::FlushRenderingCommands();
	Durin::PumpGameThreadDeferredWork();
	ASSERT_NE(CubeResult.Asset->GetResource(), nullptr);
	EXPECT_TRUE(CubeResult.Asset->GetResource()->GetTextureRHI_GameThread());
	EXPECT_FALSE(CubeResult.Asset->HasPendingRenderResourceInitialization());
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
	Durin::FTextureRHIRef Color;

	struct FRenderSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "RenderSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FRenderSkyBoxValidationFrame>(
		[&Color, CubeReference, PlatformData, HdrCubeReference, HdrPlatformData, Result](Durin::FRHICommandListImmediate& CommandList) mutable {
			FSkyValidationFrame Frame(CommandList);

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
						|| !std::ranges::equal(MipPixels, PlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels))
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
						|| !std::ranges::equal(MipPixels, HdrPlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels))
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
			Color = Durin::GDynamicRHI->RHICreateTexture(CommandList, ColorDesc);
			if (Color == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Failed to create the validation output target.";
				return;
			}
			Durin::FVolumetricCloudViewHistory History;
			(void)History.Begin({}, 1, false);
			History.SetPending(Color, 1, 1);
			History.Commit();
			Result->bTypedHistoryCandidateCommitted =
				History.CommittedPolicyKey == 1
				&& History.CommittedTexture == Color;
			(void)History.Begin({}, 2, false);
			History.SetPending(Color, 2, 1);
			History.Abort();
			Result->bTypedHistoryAbortRetainedCommitted =
				History.CommittedPolicyKey == 1
				&& History.CommittedTexture == Color
				&& History.PendingTexture == nullptr;
			History.Reset();
			Result->bTypedHistoryResetReleased =
				History.CommittedPolicyKey == 0
				&& History.CommittedTexture == nullptr;

		});
	Durin::FlushRenderingCommands();
	auto ValidateViews = [&]() {
		if (!Result->bSucceeded) return;
		auto RenderWithOptions = [&](const Durin::FSceneView& View, Durin::FByteBuffer& OutPixels,
			const Durin::FSceneViewRenderOptions& Options,
			Durin::ERenderMode RenderMode = Durin::ERenderMode::Unlit, bool bCaptureTelemetry = false) {
			Durin::FSceneView RenderView = View;
			RenderView.Settings.Mode.RenderMode = RenderMode;
			Durin::FSceneViewStatistics Statistics;
			bool bReadback = false;
			const auto Outcome = RenderPreparedSkyView(Renderer, Scene, RenderView, Color, Options, &Statistics,
				[&](Durin::FRHICommandListImmediate& Commands) {
					if (bCaptureTelemetry) Result->HybridSkyTelemetry = GHybridSkyTelemetry;
					bReadback = Durin::GDynamicRHI->RHIReadTexture2D(Commands, Color, 0, 0, OutPixels);
				}, bCaptureTelemetry);
			if (Outcome != Durin::ERenderViewResult::Success || !bReadback)
			{
				Result->bSucceeded = false;
				Result->Error = std::format("Validation view failed: result={}, readback={}", uint8(Outcome), bReadback);
				return false;
			}
			if (!Result->bCapturedFirstViewStatistics)
			{
				Result->FirstViewStatistics = Statistics;
				Result->bCapturedFirstViewStatistics = true;
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
		Durin::EnqueueRenderCommand("ValidateRejectedSkyViews", [&](Durin::FRHICommandListImmediate& CommandList) {
			FSkyValidationFrame Frame(CommandList);
			Result->InvalidViewStatistics.Summary.Triangles = 999;
			Result->InvalidViewStatistics.Summary.DrawCalls = 999;
			Result->InvalidOutputResult = Renderer.RenderView(CommandList, {&Scene, MakePrincipalAxisView(Directions[0], {}, 17, 17), nullptr, false, {}, &Result->InvalidViewStatistics});
			Durin::FSceneViewRenderOptions MissingEnvironment;
			MissingEnvironment.Environment = Durin::FViewEnvironmentOverride{};
			Result->MissingEnvironmentResult = Renderer.RenderView(CommandList, {&Scene, MakePrincipalAxisView(Directions[0], {}, 17, 17), Color, false, MissingEnvironment});
		});
		Durin::FlushRenderingCommands();
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
		const bool bRenderedHybridSky = RenderWithOptions(HybridSkyView, Result->HybridLitSky, HybridSkyOptions, Durin::ERenderMode::Lit, true);
		if (!bRenderedHybridSky) return;
		Durin::FMatrix OccluderTransform = glm::translate(
			Durin::FMatrix(1.0), Durin::FVector3(0.0, 0.0, 0.5)
		);
		OccluderTransform = glm::rotate(
			OccluderTransform, glm::pi<double>(), Durin::FVectorConstants::Right
		);
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(1), std::move(*OcclusionProxy), OccluderTransform);
		Render(MakePrincipalAxisView(Directions[4], {}, 17, 17), Result->Occluded);
	};
	ValidateViews();
	Durin::EnqueueRenderCommand("ReleaseSkyValidationColor", [&Color](Durin::FRHICommandListImmediate&) { Color = {}; });
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
		const auto DeleteResult = Durin::Testing::RemoveAssetPackageForTests(CubePath);
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
		const auto DeleteResult = Durin::Testing::RemoveAssetPackageForTests(HdrCubePath);
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
    EnqueueRenderCommand("BeginSkySetup",[](FRHICommandListImmediate& Cmd) { GDynamicRHI->RHIBeginFrame_RenderThread(Cmd); });
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
    Light->SetRefreshPolicy(true,60.0f);
    EnqueueRenderCommand("EndSkySetup",[](FRHICommandListImmediate& Cmd) { GDynamicRHI->RHIEndFrame_RenderThread(Cmd); });
    FlushRenderingCommands();
    bool Ready=false;
    uint64 Revision=0;
    uint64 PublishedRequest=0;
    FByteBuffer Before,After;
    const auto Pump=[&](bool Read, bool TickWorlds = true) {
        EnqueueRenderCommand("BeginSkyTick",[](FRHICommandListImmediate& Cmd) { ++GRenderFrameCounterRenderThread; GDynamicRHI->RHIBeginFrame_RenderThread(Cmd); });
        if (TickWorlds)
        {
            World->Tick({.DeltaSeconds=0.02f});
            OtherWorld->Tick({.DeltaSeconds=0.02f});
        }
        EnqueueRenderCommand("EndSkyTick",[&](FRHICommandListImmediate& Cmd) {
            Renderer.BeginFrame_RenderThread(Cmd);
            Ready=bool(Scene.SkyLighting->Published);
            if (Ready)
            {
                const auto& G=*Scene.SkyLighting->Published;
                Revision=G.Revision;
                PublishedRequest=G.Request;
                EXPECT_EQ(G.Radiance->GetNumMips(),8u);
                EXPECT_EQ(G.Prefiltered->GetNumMips(),8u);
                EXPECT_EQ(G.Irradiance->GetSizeX(),16u);
                if(Read) EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,G.Radiance,0,4,After));
            }
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
    };
    for(int i=0;i<150 && !Ready;++i) { Pump(false, false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    EXPECT_TRUE(Ready) << "Registration must drive first lighting without World Tick, views, or refresh-interval waits";
    Light->SetRefreshPolicy(true,0.25f);
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
        EnqueueRenderCommand("ValidateSkyHotspot",[&](FRHICommandListImmediate& Cmd) {
            GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            const auto& G=*Scene.SkyLighting->Published;
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
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
        // Visible sky uses the same radiance, then display mapping. Prepare on the host
        // before reading a target, since a Pending draw has not initialized it.
        FTextureRHIRef VisibleColor;
        EnqueueRenderCommand("CreateVisibleSkyTarget", [&](FRHICommandListImmediate& Cmd) {
            FSkyValidationFrame Frame(Cmd);
            VisibleColor=GDynamicRHI->RHICreateTexture(Cmd,FRHITextureCreateDesc::Create2D("VisibleAnalyticSky",17,17,EPixelFormat::SRGBA8_UNORM)
                .SetFlags(ETextureCreateFlags::RenderTargetable|ETextureCreateFlags::ShaderResource|ETextureCreateFlags::CPUReadback));
        });
        FlushRenderingCommands();
        EXPECT_TRUE(VisibleColor);
        if (VisibleColor)
        {
            const auto Outcome=RenderPreparedSkyView(Renderer,Scene,MakePrincipalAxisView({0,0,1},{},17,17),VisibleColor,{},nullptr,
                [&](FRHICommandListImmediate& Cmd) {
                    FByteBuffer Pixels;
                    if (!GDynamicRHI->RHIReadTexture2D(Cmd,VisibleColor,0,0,Pixels))
                    { ADD_FAILURE() << "Failed to read the prepared dynamic sky view."; return; }
                    const auto Mapped=DisplayMapping::MapSceneLinearToDisplayLinear({16,8,4},0);
                    std::array<uint8,4> Expected{0,0,0,255};
                    for(uint32 C=0;C<3;++C) Expected[C]=uint8(std::lround((1.055f*std::pow(Mapped[C],1.f/2.4f)-0.055f)*255));
                    ExpectRgbNear(Pixels,17,8,8,Expected,4);
                });
            EXPECT_EQ(Outcome,ERenderViewResult::Success);
        }
        EnqueueRenderCommand("ReleaseVisibleSkyTarget", [&](FRHICommandListImmediate&) { VisibleColor={}; });
        FlushRenderingCommands();
        Sky->SetRadianceColors({2,4,8},{2,4,8},{2,4,8},{0,0,0});
        const uint64 PreviousRevision=Revision;
        for(int i=0;i<100 && Revision==PreviousRevision;++i) { Pump(false); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        EXPECT_GT(Revision,PreviousRevision);
        EnqueueRenderCommand("ValidateSkyEnergy",[&](FRHICommandListImmediate& Cmd) {
            GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            EXPECT_TRUE(OtherScene.SkyLighting->Published);
            if (OtherScene.SkyLighting->Published)
            {
                FByteBuffer OtherPixels;
                EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Cmd,OtherScene.SkyLighting->Published->Irradiance,0,4,OtherPixels));
                EXPECT_NEAR(Half(OtherPixels,(8*16+8)*4),3.14159265f,0.01f);
                EXPECT_NE(OtherScene.SkyLighting->Published->Owner,Scene.SkyLighting->Published->Owner);
            }
            const auto& G=*Scene.SkyLighting->Published;
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
            const uint64 BeforeRecapture=PublishedRequest;
            EnqueueRenderCommand("RetainSkyConsumer",[&](FRHICommandListImmediate&) { Retained.push_back(Scene.SkyLighting->Published); });
            FlushRenderingCommands();
            Light->Recapture();
            std::this_thread::sleep_for(std::chrono::milliseconds(270));
            // Frame dispatch is asynchronous. Wait for this generation's
            // observable result before retaining the next distinct generation.
            for(int Poll=0;Poll<100 && PublishedRequest==BeforeRecapture && !Backpressured;++Poll)
            {
                Pump(false);
                Backpressured=Light->GetUpdateStatus()->State.load()==ESkyLightUpdateState::Backpressure;
                if(PublishedRequest==BeforeRecapture && !Backpressured)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        EXPECT_TRUE(Backpressured);
        EnqueueRenderCommand("CheckLastGoodSky",[&](FRHICommandListImmediate&) {
            EXPECT_EQ(Scene.SkyLighting->Published,Retained.back());
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
        EnqueueRenderCommand("ReportSkyTiming",[&](FRHICommandListImmediate&) {
            const auto Selected=Scene.GetSkyLight_RenderThread();
            std::cout << "Dynamic sky GPU update: " << Selected->UpdateStatus->UpdateMilliseconds.load() << " ms\n";
        });
        FlushRenderingCommands();
        // Invalidate while the scene is inactive: old ownership must disappear
        // without waiting for another world tick or view submission.
        EXPECT_TRUE(Renderer.RequestResourceInvalidation(ERendererResourceInvalidationCause::Device).bSuccess);
        EnqueueRenderCommand("CheckSkyInvalidation",[&](FRHICommandListImmediate&) {
            EXPECT_FALSE(Scene.SkyLighting->Published);
            EXPECT_FALSE(Scene.SkyLighting->Job);
            EXPECT_FALSE(OtherScene.SkyLighting->Published);
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
    std::weak_ptr<const FSkyLightingGeneration> LastSceneLease;
    EnqueueRenderCommand("ObserveLastSkyLease", [&](FRHICommandListImmediate&) {
        LastSceneLease = OtherScene.SkyLighting->Job ? OtherScene.SkyLighting->Job->Output : OtherScene.SkyLighting->Published;
    });
    FlushRenderingCommands();
    EXPECT_FALSE(LastSceneLease.expired());
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
    for (int Poll=0; Poll<150 && !LastSceneLease.expired(); ++Poll)
    {
        EnqueueRenderCommand("RetireWithoutScenes", [&](FRHICommandListImmediate& Cmd) {
            ++GRenderFrameCounterRenderThread;
            GDynamicRHI->RHIBeginFrame_RenderThread(Cmd);
            Renderer.BeginFrame_RenderThread(Cmd);
            GDynamicRHI->RHIEndFrame_RenderThread(Cmd);
        });
        FlushRenderingCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(LastSceneLease.expired());
    FlushRenderingCommands();
}
