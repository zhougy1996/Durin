#include "SkyBoxTestSupport.h"
#include "TextureCubeSourceTranslation.h"

TEST(FSkyBoxVulkanTests, SamplesPanoramaFacesMipsBoundariesAndHdrWithoutParallax)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints());
	std::vector<Durin::PathUtilities::FMountPoint> MountDefinitions(
		Durin::PathUtilities::GetRegisteredMountPoints().begin(),
		Durin::PathUtilities::GetRegisteredMountPoints().end());
	const std::filesystem::path AssetRoot =
		Durin::Testing::GetTestWorkDirectory() / "SkyBoxAssets";
	Durin::Testing::RemoveTestWorkDirectory(AssetRoot);
	std::filesystem::create_directories(AssetRoot);
	MountDefinitions.push_back({
		.VirtualRoot = "/SkyBoxAssetTests/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.Root = AssetRoot,
		.bAutoScan = true,
		.bAuthoringWritable = true});
	Durin::PathUtilities::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("StandardAssetImport");
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	Durin::FRendererModule Renderer;
	Durin::FScene Scene;
	struct FBeginSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginSkyBoxValidationFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
	});
	Renderer.StartupModule();

	Durin::Asset::Import::Standard::FTextureCubeImportResult CubeResult = Durin::Asset::Import::Standard::ImportTextureCubePanorama(
		GetSkyBoxPanoramaFixture("AnalyticalLDR.tga").generic_string(),
		"/SkyBoxAssetTests/VulkanPanoramaLdr",
		{.FaceDimension = 64});
	ASSERT_TRUE(CubeResult) << CubeResult.Message;
	auto CubeReference = CubeResult.Asset->GetTextureReferenceRHI();
	ASSERT_NE(CubeReference, nullptr);
	auto PlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*CubeResult.Asset->GetPlatformData());
	Durin::Asset::Import::Standard::FTextureCubeImportResult HdrCubeResult = Durin::Asset::Import::Standard::ImportTextureCubePanorama(
		GetSkyBoxPanoramaFixture("AnalyticalHDR.hdr").generic_string(),
		"/SkyBoxAssetTests/VulkanPanoramaHdr",
		{.FaceDimension = 64, .ExposureEV = 1.0f});
	ASSERT_TRUE(HdrCubeResult) << HdrCubeResult.Message;
	auto HdrCubeReference = HdrCubeResult.Asset->GetTextureReferenceRHI();
	ASSERT_NE(HdrCubeReference, nullptr);
	auto HdrPlatformData = std::make_shared<Durin::FTextureCubePlatformData>(*HdrCubeResult.Asset->GetPlatformData());
	std::array<std::array<Durin::uint8, 4>, Durin::TextureCubeFaceCount> SourceColors;
	std::array<std::array<Durin::uint8, 4>, Durin::TextureCubeFaceCount> HdrSourceColors;
	for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
	{
		SourceColors[FaceIndex] = GetSourceColor(
			*CubeResult.Asset, static_cast<Durin::ETextureCubeFace>(FaceIndex), 32, 32);
		HdrSourceColors[FaceIndex] = GetSourceColor(
			*HdrCubeResult.Asset, static_cast<Durin::ETextureCubeFace>(FaceIndex), 32, 32);
	}
	auto* OcclusionMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* OcclusionMaterial = Durin::NewObject<Durin::DMaterial>(nullptr, "SkyBoxOcclusionMaterial");
	Durin::FMaterialStaticProperties OcclusionProperties;
	OcclusionProperties.bTwoSided = true;
	ASSERT_TRUE(OcclusionMaterial->SetStaticProperties(OcclusionProperties));
	OcclusionMaterial->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), {1.0, 0.0, 0.0});
	auto* OcclusionComponent = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SkyBoxOcclusionMesh");
	OcclusionComponent->SetStaticMesh(OcclusionMesh);
	OcclusionComponent->SetMaterial(OcclusionMaterial);
	auto OcclusionProxy = std::make_shared<std::unique_ptr<Durin::FPrimitiveSceneProxy>>(
		OcclusionComponent->CreateSceneProxy());
	ASSERT_NE(*OcclusionProxy, nullptr);

	Durin::FSkyBoxSceneData SkyBox;
	SkyBox.SceneId = Durin::FGuid(1, 0, 0, 0);
	SkyBox.SelectionKey = "VulkanSky";
	SkyBox.InstanceId = 1;
	SkyBox.TextureReference = CubeReference;
	PublishSkyBox(Scene, SkyBox);

	struct FEndSkyBoxValidationSetupFrame
	{
		static constexpr auto GetName() -> const char* { return "EndSkyBoxValidationSetupFrame"; }
	};
	Durin::EnqueueRenderCommand<FEndSkyBoxValidationSetupFrame>([](Durin::FRHICommandListImmediate& CommandList) {
		Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
	});
	Durin::FlushRenderingCommands();

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
			Durin::FRHICommandListImmediate&) {
			ObservedCubeTarget->store(
				CubeReference->GetReferencedTexture_RenderThread(),
				std::memory_order_release);
		});
	Durin::FlushRenderingCommands();
	Durin::FRHITexture* InitialCubeTarget =
		ObservedCubeTarget->load(std::memory_order_acquire);
	ASSERT_NE(InitialCubeTarget, nullptr);

	std::string RebuildError;
	ASSERT_TRUE(CubeResult.Asset->RebuildPlatformData(RebuildError))
		<< RebuildError;
	EXPECT_EQ(CubeResult.Asset->GetTextureReferenceRHI(), CubeReference);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(
		CubeResult.Asset->GetRenderResourceState(),
		Durin::ERenderResourceState::Ready);
	EXPECT_EQ(
		CubeResult.Asset->GetAppliedRenderRevision(),
		CubeResult.Asset->GetBuildRevision());
	struct FObserveReplacementCubeTarget
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveReplacementCubeTarget";
		}
	};
	Durin::EnqueueRenderCommand<FObserveReplacementCubeTarget>(
		[CubeReference, ObservedCubeTarget](
			Durin::FRHICommandListImmediate&) {
			ObservedCubeTarget->store(
				CubeReference->GetReferencedTexture_RenderThread(),
				std::memory_order_release);
		});
	Durin::FlushRenderingCommands();
	Durin::FRHITexture* ReplacementCubeTarget =
		ObservedCubeTarget->load(std::memory_order_acquire);
	ASSERT_NE(ReplacementCubeTarget, nullptr);
	EXPECT_NE(ReplacementCubeTarget, InitialCubeTarget);

	struct FValidationResult
	{
		bool bSucceeded = true;
		std::string Error;
		std::array<std::vector<Durin::uint8>, Durin::TextureCubeFaceCount> PrincipalAxes;
		std::array<std::vector<Durin::uint8>, Durin::TextureCubeFaceCount> HdrPrincipalAxes;
		std::array<std::vector<Durin::uint8>, 2> LongitudeSeam;
		std::array<std::vector<Durin::uint8>, 2> FaceBoundary;
		std::vector<Durin::uint8> Translated;
		std::vector<Durin::uint8> ComponentRotated;
		std::vector<Durin::uint8> ExplicitOverride;
		std::vector<Durin::uint8> Letterboxed;
		std::vector<Durin::uint8> Occluded;
		Durin::ERenderViewResult InvalidOutputResult =
			Durin::ERenderViewResult::Success;
		Durin::ERenderViewResult MissingEnvironmentResult =
			Durin::ERenderViewResult::Success;
		Durin::FSceneViewStatistics FirstViewStatistics;
		Durin::FSceneViewStatistics InvalidViewStatistics;
		bool bCapturedFirstViewStatistics = false;
	};
	auto Result = std::make_shared<FValidationResult>();

	struct FRenderSkyBoxValidationFrame
	{
		static constexpr auto GetName() -> const char* { return "RenderSkyBoxValidationFrame"; }
	};
	Durin::EnqueueRenderCommand<FRenderSkyBoxValidationFrame>(
		[&Renderer, &Scene, CubeReference, PlatformData,
			HdrCubeReference, HdrPlatformData, Result, OcclusionProxy]
		(Durin::FRHICommandListImmediate& CommandList) {
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
			for (Durin::uint32 FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
			{
				for (Durin::uint32 MipIndex = 0; MipIndex < PlatformData->Faces[FaceIndex].Mips.size(); ++MipIndex)
				{
					std::vector<Durin::uint8> MipPixels;
					if (!Durin::GDynamicRHI->RHIReadTexture2D(
						CommandList, CubeTexture, MipIndex, FaceIndex, MipPixels)
						|| MipPixels != PlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels)
					{
						Result->bSucceeded = false;
						Result->Error = std::format(
							"Cube readback mismatch for face {} mip {}.", FaceIndex, MipIndex);
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
			for (Durin::uint32 FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
			{
				for (Durin::uint32 MipIndex = 0; MipIndex < HdrPlatformData->Faces[FaceIndex].Mips.size(); ++MipIndex)
				{
					std::vector<Durin::uint8> MipPixels;
					if (!Durin::GDynamicRHI->RHIReadTexture2D(
						CommandList, HdrCubeTexture, MipIndex, FaceIndex, MipPixels)
						|| MipPixels != HdrPlatformData->Faces[FaceIndex].Mips[MipIndex].Pixels)
					{
						Result->bSucceeded = false;
						Result->Error = std::format(
							"HDR-derived cube readback mismatch for face {} mip {}.", FaceIndex, MipIndex);
						return;
					}
				}
			}

			Durin::FRHITextureCreateDesc ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
				"SkyBoxValidationColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Color = Durin::GDynamicRHI->RHICreateTexture(CommandList, ColorDesc);
			if (Color == nullptr)
			{
				Result->bSucceeded = false;
				Result->Error = "Failed to create the validation output target.";
				return;
			}

				auto RenderWithOptions = [&](const Durin::FSceneView& View,
				std::vector<Durin::uint8>& OutPixels,
				const Durin::FSceneViewRenderOptions& Options) {
				Durin::FSceneView RenderView = View;
				RenderView.Settings.RenderMode = Durin::ERenderMode::Unlit;
				Durin::FSceneViewStatistics Statistics;
				if (Renderer.RenderView(
						CommandList, &Scene, RenderView, Color, false, Options,
						&Statistics)
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
				std::vector<Durin::uint8>& OutPixels) {
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
			constexpr double EdgeOffset = 0.02;
			if (!Render(MakePrincipalAxisView({-1.0, EdgeOffset, 0.0}, {}, 17, 17), Result->LongitudeSeam[0])) return;
			if (!Render(MakePrincipalAxisView({-1.0, -EdgeOffset, 0.0}, {}, 17, 17), Result->LongitudeSeam[1])) return;
			if (!Render(MakePrincipalAxisView({1.0, 1.0 - EdgeOffset, 0.0}, {}, 17, 17), Result->FaceBoundary[0])) return;
			if (!Render(MakePrincipalAxisView({1.0 - EdgeOffset, 1.0, 0.0}, {}, 17, 17), Result->FaceBoundary[1])) return;
			if (!Render(MakePrincipalAxisView(Directions[0], {19.0, -7.0, 4.0}, 17, 17), Result->Translated)) return;

			Durin::FSkyBoxSceneData RotatedSky;
			RotatedSky.SceneId = Durin::FGuid(1, 0, 0, 0);
			RotatedSky.SelectionKey = "VulkanSky";
			RotatedSky.InstanceId = 1;
			RotatedSky.TextureReference = CubeReference;
			RotatedSky.Rotation = glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up);
			PublishSkyBox(Scene, RotatedSky);
			if (!Render(MakePrincipalAxisView(Directions[0], {}, 17, 17), Result->ComponentRotated)) return;

			RotatedSky.TextureReference = HdrCubeReference;
			RotatedSky.Rotation = glm::identity<Durin::FQuat>();
			PublishSkyBox(Scene, RotatedSky);
			Durin::FSceneViewRenderOptions OverrideOptions;
			OverrideOptions.Environment = Durin::FViewEnvironmentOverride{
				.TextureReference = CubeReference};
			if (!RenderWithOptions(
					MakePrincipalAxisView(Directions[0], {}, 17, 17),
					Result->ExplicitOverride,
					OverrideOptions)) return;
			Result->InvalidViewStatistics.Triangles = 999;
			Result->InvalidViewStatistics.DrawCalls = 999;
			Result->InvalidOutputResult = Renderer.RenderView(
				CommandList,
				&Scene,
				MakePrincipalAxisView(Directions[0], {}, 17, 17),
				nullptr,
				false,
				{},
				&Result->InvalidViewStatistics);
			Durin::FSceneViewRenderOptions MissingEnvironment;
			MissingEnvironment.Environment = Durin::FViewEnvironmentOverride{};
			Result->MissingEnvironmentResult = Renderer.RenderView(
				CommandList,
				&Scene,
				MakePrincipalAxisView(Directions[0], {}, 17, 17),
				Color,
				false,
				MissingEnvironment);
			for (size_t FaceIndex = 0; FaceIndex < Directions.size(); ++FaceIndex)
			{
				if (!Render(MakePrincipalAxisView(
					Directions[FaceIndex], {}, 17, 17), Result->HdrPrincipalAxes[FaceIndex])) return;
			}

			RotatedSky.TextureReference = CubeReference;
			PublishSkyBox(Scene, RotatedSky);
			Durin::FSceneView LetterboxView = MakePrincipalAxisView(Directions[0], {}, 17, 17);
			LetterboxView.AspectRatioConstraint = 0.5f;
			if (!Render(LetterboxView, Result->Letterboxed)) return;

			RotatedSky.Rotation = glm::identity<Durin::FQuat>();
			PublishSkyBox(Scene, RotatedSky);
			Durin::FMatrix OccluderTransform = glm::translate(
				Durin::FMatrix(1.0), Durin::FVector3(0.0, 0.0, 0.5));
			OccluderTransform = glm::rotate(
				OccluderTransform, glm::pi<double>(), Durin::FVectorConstants::Right);
			Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(1), std::move(*OcclusionProxy), OccluderTransform);
			Render(MakePrincipalAxisView(Directions[4], {}, 17, 17), Result->Occluded);
		});
	Durin::FlushRenderingCommands();

	EXPECT_TRUE(Result->bSucceeded) << Result->Error;
	if (Result->bSucceeded)
	{
		EXPECT_EQ(
			Result->InvalidOutputResult,
			Durin::ERenderViewResult::InvalidOutput);
		EXPECT_EQ(
			Result->MissingEnvironmentResult,
			Durin::ERenderViewResult::RequiredEnvironmentUnavailable);
		EXPECT_TRUE(Result->bCapturedFirstViewStatistics);
		EXPECT_EQ(Result->FirstViewStatistics.VisiblePrimitives, 0u);
		EXPECT_EQ(Result->FirstViewStatistics.Triangles, 0u);
		EXPECT_GT(Result->FirstViewStatistics.DrawCalls, 0u);
		EXPECT_EQ(Result->InvalidViewStatistics, Durin::FSceneViewStatistics{});
		EXPECT_EQ(Result->ExplicitOverride, Result->PrincipalAxes[0]);
		for (size_t FaceIndex = 0; FaceIndex < Durin::TextureCubeFaceCount; ++FaceIndex)
		{
			SCOPED_TRACE(std::format("principal face {}", FaceIndex));
			ExpectRgbNear(
				Result->PrincipalAxes[FaceIndex],
				17,
				8,
				8,
				SourceColors[FaceIndex],
				12);
			ExpectRgbNear(
				Result->HdrPrincipalAxes[FaceIndex],
				17,
				8,
				8,
				HdrSourceColors[FaceIndex],
				12);
		}
		ExpectRgbMatch(Result->LongitudeSeam[0], Result->LongitudeSeam[1], 17, 8, 8, 12);
		ExpectRgbMatch(Result->FaceBoundary[0], Result->FaceBoundary[1], 17, 8, 8, 16);
		ExpectRgbMatch(Result->Translated, Result->PrincipalAxes[0], 17, 8, 8);
		EXPECT_EQ(FindClosestCenterRgb(Result->ComponentRotated, Result->PrincipalAxes, 17), 3u);
		ExpectRgbNear(Result->Letterboxed, 17, 1, 8, {0, 0, 0, 255}, 2);
		EXPECT_EQ(FindClosestCenterRgb(Result->Letterboxed, Result->PrincipalAxes, 17), 0u);
		ExpectRgbNear(Result->Occluded, 17, 8, 8, {255, 0, 0, 255}, 8);
	}

	Scene.Release();
	Durin::FAssetPath CubePath;
	if (Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/VulkanPanoramaLdr", CubePath))
	{
		EXPECT_TRUE(Durin::Asset::DeleteAsset(CubePath));
	}
	else
	{
		ADD_FAILURE() << "Failed to create the Vulkan cube cleanup path.";
	}
	Durin::FAssetPath HdrCubePath;
	if (Durin::FAssetPath::TryCreate("/SkyBoxAssetTests/VulkanPanoramaHdr", HdrCubePath))
	{
		EXPECT_TRUE(Durin::Asset::DeleteAsset(HdrCubePath));
	}
	else
	{
		ADD_FAILURE() << "Failed to create the Vulkan HDR cube cleanup path.";
	}
	Durin::MarkAsGarbage(OcclusionComponent);
	Durin::MarkAsGarbage(OcclusionMesh);
	Durin::MarkAsGarbage(OcclusionMaterial);
	Durin::CollectGarbage();
	SkyBox.TextureReference = nullptr;
	struct FRetireSkyBoxValidationResource
	{
		static constexpr auto GetName() -> const char* { return "RetireSkyBoxValidationResource"; }
	};
	Durin::EnqueueRenderCommand<FRetireSkyBoxValidationResource>(
		[Reference = std::move(CubeReference),
			HdrReference = std::move(HdrCubeReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Renderer.ShutdownModule();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
