#include "MaterialTestSupport.h"
#include "TextureCubeSourceTranslation.h"
#include "Console/ConsoleCommand.h"
#include "DefaultTextures.h"
#include "DynamicRHI.h"
#include "Renderers/DisplayMapping.h"
#include "Modules/ModuleManager.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "MonaTestFixtures.h"
#include "NativeTestSupport.h"
#include "PBRLighting.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "AssetForgeProviders.h"
#include "AssetForgeProviderTestFixture.h"
#include "AssetForgeAuthoringFeatures.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailPreviewScene.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <limits>

namespace
{
	auto MapSrgbChannelThroughDisplay(Durin::uint8 Source) -> Durin::uint8
	{
		const float Encoded = static_cast<float>(Source) / 255.0f;
		const float Linear = Encoded <= 0.04045f
			? Encoded / 12.92f
			: std::pow((Encoded + 0.055f) / 1.055f, 2.4f);
		const float Mapped = Durin::DisplayMapping::MapSceneLinearToDisplayLinear(
			{Linear, Linear, Linear}, 0.0f).x;
		const float DisplayEncoded = Mapped <= 0.0031308f
			? 12.92f * Mapped
			: 1.055f * std::pow(Mapped, 1.0f / 2.4f) - 0.055f;
		return static_cast<Durin::uint8>(std::lround(
			std::clamp(DisplayEncoded, 0.0f, 1.0f) * 255.0f));
	}

}

TEST(FMaterialVulkanTests, RenderedThumbnailPreviewSceneCapturesResolvedMaterialDifferences)
{
	static Durin::FModuleTestOwner AuthoringContext("MaterialVulkanTests.Authoring");
	static Durin::Asset::Forge::FAssetForgeAuthoringFeatures AuthoringFeatures;
	static auto StaticMeshAuthoring =
		AuthoringContext.RegisterFeature<Durin::IStaticMeshAuthoringFeature>(AuthoringFeatures);
	static auto Texture2DAuthoring =
		AuthoringContext.RegisterFeature<Durin::ITexture2DAuthoringFeature>(AuthoringFeatures);
	static auto TextureCubeAuthoring =
		AuthoringContext.RegisterFeature<Durin::ITextureCubeAuthoringFeature>(AuthoringFeatures);
	(void)StaticMeshAuthoring;
	(void)Texture2DAuthoring;
	(void)TextureCubeAuthoring;
	Durin::Tests::FScopedAssetForgeProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	InitializeDObjectSystem();
	std::string StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle StaticMeshProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider>(),
			StaticMeshProviderError);
	ASSERT_TRUE(StaticMeshProvider) << StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle MaterialProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
				Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()),
			StaticMeshProviderError);
	ASSERT_TRUE(MaterialProvider) << StaticMeshProviderError;
	Durin::Editor::FAssetThumbnailProviderRegistrationHandle MaterialInstanceProvider =
		Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
				Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
			StaticMeshProviderError);
	ASSERT_TRUE(MaterialInstanceProvider) << StaticMeshProviderError;
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog());
	const std::filesystem::path TextureMount =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan";
	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan.png";
	Durin::Testing::RemoveTestWorkDirectory(TextureMount);
	std::filesystem::create_directories(TextureMount);
	WriteMaterialTextureFixture(TextureSource);
	std::vector<Durin::PathUtilities::FMountPoint> MountDefinitions(
		Durin::PathUtilities::GetRegisteredMountPoints().begin(),
		Durin::PathUtilities::GetRegisteredMountPoints().end());
	MountDefinitions.push_back({
		.VirtualRoot = "/MaterialThumbnailVulkan/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.Root = TextureMount,
		.bAutoScan = true,
		.bAuthoringWritable = true});
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry(MountDefinitions);
	ASSERT_TRUE(MountRegistry.IsValid()) << MountRegistry.GetError();
	Durin::FAssetPath SpherePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Editor::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::Editor::FRetainedAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(
		SpherePath, PreloadedSphere, Error)) << Error;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	// A direct target run may leave deferred texture releases from earlier
	// CPU-only material cases. Drain them before creating the Vulkan device so
	// no stale game-thread owner survives into the RHI-backed portion.
	Durin::InitRenderingThread();
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::FRHIInitializationContext::Headless());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	struct FBeginRenderedThumbnailFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginRenderedThumbnailFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginRenderedThumbnailFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
		});

	FMaterialTestEngine Engine;
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("MaterialRendererTest");
	RendererLifecycle.Start(Renderer);
	Engine.SetTestRendererModule(&Renderer);
	Durin::GEngine = &Engine;

	Durin::Editor::FRenderedAssetThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	Durin::DStaticMesh* CaptureMesh = nullptr;
	Durin::DStaticMesh* CaptureSphere = nullptr;
	Durin::DMaterial* CaptureMaterial = nullptr;
	Durin::DMaterialInstance* CaptureInstance = nullptr;
	Durin::DMaterialInstance* InheritedInstance = nullptr;
	Durin::DTextureCube* CaptureCube = nullptr;
	Durin::FRHITextureReferenceRef CaptureCubeReference;
	Durin::FAssetPath CaptureTexturePath;
	Durin::FAssetPath DataTexturePath;
	Durin::FAssetPath NormalTexturePath;
	Durin::FAssetPath CaptureCubePath;
	Durin::FAssetPath StaticMeshFixturePath;
	Durin::FAssetPath StaticMeshMaterialPath;
	Durin::DStaticMesh* LowRoughnessMesh =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DMaterial* LowRoughnessMaterial =
		Durin::NewObject<Durin::DMaterial>(
			nullptr, "LowRoughnessRenderedReferenceMaterial");
	ASSERT_NE(LowRoughnessMesh, nullptr);
	ASSERT_NE(LowRoughnessMaterial, nullptr);
	ASSERT_TRUE(LowRoughnessMaterial->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.5)));
	ASSERT_TRUE(LowRoughnessMaterial->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.0f));
	Durin::Editor::FRenderedAssetThumbnailVisualContract AlignedContract = Contract;
	AlignedContract.CameraDirectionX = 0.001f;
	AlignedContract.CameraDirectionY = 0.0f;
	AlignedContract.CameraDirectionZ = 1.0f;
	AlignedContract.KeyLightDirectionX = 0.0f;
	AlignedContract.KeyLightDirectionY = 0.0f;
	AlignedContract.KeyLightDirectionZ = -1.0f;
	{
		Durin::Tests::FRenderedAssetThumbnailTestPool AlignedPool(
			AlignedContract);
		ASSERT_TRUE(AlignedPool.IsAvailable()) << AlignedPool.GetDiagnostic();
		auto CaptureAligned = [&](float Roughness) {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(LowRoughnessMaterial->SetScalarParameterValue(
				Durin::MaterialParameters::RoughnessName(), Roughness));
			EXPECT_TRUE(AlignedPool.SetMaterial(
				LowRoughnessMesh,
				LowRoughnessMaterial,
				Durin::FTransform(),
				Error)) << Error;
			EXPECT_TRUE(AlignedPool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				AlignedPool.PollCapture(Pixels, Error),
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			AlignedPool.Reset();
			return Pixels;
		};
		constexpr std::array RoughnessSweep{0.045f, 0.1f, 0.2f, 0.5f, 1.0f};
		const Durin::uint32 DisplayMappedHighlightThreshold =
			3u * MapSrgbChannelThroughDisplay(250u);
		std::array<Durin::uint32, 5> Peaks{};
		std::array<Durin::uint32, 5> SaturatedPixelCounts{};
		for (size_t Index = 0; Index < RoughnessSweep.size(); ++Index)
		{
			const std::vector<Durin::uint8> Pixels =
				CaptureAligned(RoughnessSweep[Index]);
			for (size_t Pixel = 0; Pixel < Pixels.size(); Pixel += 4)
			{
				const Durin::uint32 Brightness =
					static_cast<Durin::uint32>(Pixels[Pixel])
					+ Pixels[Pixel + 1] + Pixels[Pixel + 2];
				Peaks[Index] = std::max(Peaks[Index], Brightness);
				SaturatedPixelCounts[Index] +=
					Brightness >= DisplayMappedHighlightThreshold ? 1u : 0u;
			}
		}
		EXPECT_GE(Peaks[0], DisplayMappedHighlightThreshold);
		EXPECT_GE(Peaks[1], DisplayMappedHighlightThreshold);
		EXPECT_GE(Peaks[2], DisplayMappedHighlightThreshold);
		EXPECT_LT(Peaks[3], 600u);
		EXPECT_LT(Peaks[4], 600u);
		EXPECT_GT(SaturatedPixelCounts[0], 0u);
		EXPECT_LT(SaturatedPixelCounts[0], SaturatedPixelCounts[1]);
		EXPECT_LT(SaturatedPixelCounts[1], SaturatedPixelCounts[2]);
		EXPECT_EQ(SaturatedPixelCounts[3], 0u);
		EXPECT_EQ(SaturatedPixelCounts[4], 0u);
	}
	{
		Durin::Tests::FRenderedAssetThumbnailTestPool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		Durin::DStaticMesh* Sphere = Pool.GetSphereMesh();
		ASSERT_NE(Sphere, nullptr);
		CaptureSphere = Sphere;
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		CaptureMesh = Durin::DStaticMesh::CreateDebugTriangle();
		ASSERT_NE(CaptureMesh, nullptr);
		CaptureMaterial = Durin::NewObject<Durin::DMaterial>(
			nullptr, "RenderedThumbnailCaptureMaterial");
		CaptureInstance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "RenderedThumbnailCaptureInstance");
		InheritedInstance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "RenderedThumbnailInheritedInstance");
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.8, 0.15, 0.05)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.2f));
		ASSERT_TRUE(CaptureInstance->SetParent(CaptureMaterial));
		ASSERT_TRUE(CaptureInstance->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.05, 0.2, 0.8)));
		ASSERT_TRUE(CaptureInstance->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.8f));
		ASSERT_TRUE(InheritedInstance->SetParent(CaptureMaterial));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Preview", CaptureTexturePath));
		const Durin::FTexture2DImportResult TextureResult =
			Durin::Asset::Forge::ImportTexture2DAsset(
				TextureSource.generic_string(), CaptureTexturePath.ToString());
		ASSERT_TRUE(TextureResult) << TextureResult.Message;
		ASSERT_NE(TextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_TRUE(TextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			TextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC3_UNORM_SRGB);
		EXPECT_GT(TextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Data", DataTexturePath));
		const Durin::FTexture2DImportResult DataTextureResult =
			Durin::Asset::Forge::ImportTexture2DAsset(
				TextureSource.generic_string(), DataTexturePath.ToString());
		ASSERT_TRUE(DataTextureResult) << DataTextureResult.Message;
		ASSERT_TRUE(Durin::Asset::Forge::SetTexture2DUsage(
			*DataTextureResult.Asset, Durin::ETextureUsage::DataMask, Error)) << Error;
		ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(
			*DataTextureResult.Asset, 10.0))
			<< Durin::Asset::Build::GetTexture2DBuildDiagnostic(*DataTextureResult.Asset).Message;
		ASSERT_NE(DataTextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_FALSE(DataTextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			DataTextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC7_UNORM);
		EXPECT_GT(DataTextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Normal", NormalTexturePath));
		const Durin::FTexture2DImportResult NormalTextureResult =
			Durin::Asset::Forge::ImportTexture2DAsset(
				TextureSource.generic_string(), NormalTexturePath.ToString());
		ASSERT_TRUE(NormalTextureResult) << NormalTextureResult.Message;
		ASSERT_TRUE(Durin::Asset::Forge::SetTexture2DUsage(
			*NormalTextureResult.Asset, Durin::ETextureUsage::Normal, Error)) << Error;
		ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(
			*NormalTextureResult.Asset, 10.0))
			<< Durin::Asset::Build::GetTexture2DBuildDiagnostic(*NormalTextureResult.Asset).Message;
		ASSERT_NE(NormalTextureResult.Asset->GetPlatformData(), nullptr);
		EXPECT_FALSE(NormalTextureResult.Asset->IsSRGB());
		EXPECT_EQ(
			NormalTextureResult.Asset->GetPlatformData()->PixelFormat,
			Durin::EPixelFormat::BC5_UNORM);
		EXPECT_GT(NormalTextureResult.Asset->GetPlatformData()->Mips.size(), 1u);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), TextureResult.Asset));
		Durin::FlushRenderingCommands();

		auto Capture = [&](
			Durin::DMaterialInterface* Material,
			const Durin::FTransform& Transform = Durin::FTransform()) {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(Pool.SetMaterial(
				CaptureMesh, Material, Transform, Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};
		const std::vector<Durin::uint8> MaterialPixels =
			Capture(CaptureMaterial);
		Durin::FMaterialStaticProperties TwoSidedProperties =
			CaptureMaterial->GetStaticProperties();
		TwoSidedProperties.bTwoSided = true;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(TwoSidedProperties));
		const std::vector<Durin::uint8> TwoSidedFrontPixels =
			Capture(CaptureMaterial);
		Durin::FTransform BackFaceTransform;
		BackFaceTransform.Scale3D.z = -1.0;
		const std::vector<Durin::uint8> TwoSidedBackPixels =
			Capture(CaptureMaterial, BackFaceTransform);
		TwoSidedProperties.bTwoSided = false;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(TwoSidedProperties));
		const std::vector<Durin::uint8> InstancePixels =
			Capture(CaptureInstance);
		const std::vector<Durin::uint8> InheritedBeforePixels =
			Capture(InheritedInstance);
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.15, 0.7, 0.2)));
		const std::vector<Durin::uint8> InheritedAfterPixels =
			Capture(InheritedInstance);

		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/SM_ThumbnailPreview", StaticMeshFixturePath));
		Durin::DStaticMesh* StaticMeshFixture = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(
			StaticMeshFixturePath, StaticMeshFixture)) << Error;
		ASSERT_NE(StaticMeshFixture, nullptr);
		Durin::Asset::Build::FStaticMeshImportedData ImportedMesh;
		ImportedMesh.MaterialSlots.push_back({
			.Name = "Default",
			.SourceMaterialIndex = 0,
			.SourceName = "Default"});
		Durin::Asset::Build::FStaticMeshImportedMesh& ImportedSection =
			ImportedMesh.Meshes.emplace_back();
		ImportedSection.Name = "ThumbnailTetrahedron";
		ImportedSection.Positions = {
			Durin::FVector3f(-0.6f, -0.5f, -0.4f),
			Durin::FVector3f(0.7f, -0.4f, -0.3f),
			Durin::FVector3f(0.0f, 0.8f, -0.2f),
			Durin::FVector3f(0.1f, 0.0f, 0.9f)};
		ImportedSection.Indices = {
			0, 2, 1,
			0, 1, 3,
			1, 2, 3,
			2, 0, 3};
		ImportedSection.SourceMaterialIndex = 0;
		ASSERT_TRUE(Durin::Asset::Build::FStaticMeshBuildOperations::BuildAndPublishImported(
			*StaticMeshFixture, ImportedMesh,
			{
				.SourcePath = {.Path = "/MaterialThumbnailVulkan/SM_ThumbnailPreview.fixture"},
				.SourceContentHash = "0123456789abcdef0123456789abcdef",
				.ImporterId = "MaterialThumbnailTest",
				.ImporterVersion = 1,
				.ImportSettings = Durin::FStaticMeshImportSettings::MakeDurin()},
			"StaticMesh thumbnail preview test fixture",
			Error)) << Error;
		ASSERT_TRUE(StaticMeshFixture->SetImportedDefaultMaterial(
			0, CaptureMaterial, Error)) << Error;
		StaticMeshFixture->InitResources();
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			StaticMeshFixture->GetRenderResourceStatus().Readiness,
			Durin::EStaticMeshRenderResourceReadiness::Ready);
		const std::optional<Durin::FBox> StaticMeshBounds =
			StaticMeshFixture->GetLOD0LocalBounds();
		ASSERT_TRUE(StaticMeshBounds.has_value());
		Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailView StaticMeshView;
		ASSERT_TRUE(Durin::Editor::StaticMesh::CalculateStaticMeshAssetThumbnailView({
			.LocalBounds = *StaticMeshBounds,
			.OutputAspectRatio = 1.0,
			.VerticalFieldOfViewDegrees = Contract.VerticalFieldOfViewDegrees,
			.CameraDirection = Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput{}
				.CameraDirection},
			StaticMeshView,
			Error)) << Error;
		auto CaptureStaticMesh = [&] {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(Pool.SetStaticMesh(
				StaticMeshFixture, StaticMeshView, Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(
				Error, Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailContract::bOutputOpaque))
				<< Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};
		const std::vector<Durin::uint8> StaticMeshPixels = CaptureStaticMesh();
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.85, 0.12, 0.18)));
		const std::vector<Durin::uint8> RecoloredStaticMeshPixels = CaptureStaticMesh();
		ASSERT_EQ(StaticMeshPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(RecoloredStaticMeshPixels.size(), StaticMeshPixels.size());
		EXPECT_NE(StaticMeshPixels, RecoloredStaticMeshPixels);
		Durin::uint32 GeometryPixels = 0;
		for (Durin::uint32 Y = 0; Y < 64; ++Y)
		{
			for (Durin::uint32 X = 0; X < 64; ++X)
			{
				const size_t Pixel = (Y * 64u + X) * 4u;
				GeometryPixels += StaticMeshPixels[Pixel + 3] != 0u ? 1u : 0u;
				if (X == 0 || Y == 0 || X == 63 || Y == 63)
					EXPECT_EQ(StaticMeshPixels[Pixel + 3], 0u);
			}
		}
		EXPECT_GT(GeometryPixels, 64u);
		EXPECT_LT(GeometryPixels, 64u * 64u);

		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/M_StaticMeshThumbnail",
			StaticMeshMaterialPath));
		Durin::DMaterial* StaticMeshAssetMaterial = nullptr;
		ASSERT_TRUE(Durin::Asset::CreateAsset(
			StaticMeshMaterialPath,
			StaticMeshAssetMaterial));
		ASSERT_NE(StaticMeshAssetMaterial, nullptr);
		ASSERT_TRUE(StaticMeshAssetMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.85, 0.12, 0.18)));
		ASSERT_TRUE(Durin::Asset::SavePackage(StaticMeshAssetMaterial->GetPackage()));
		ASSERT_TRUE(StaticMeshFixture->SetImportedDefaultMaterial(
			0, StaticMeshAssetMaterial, Error)) << Error;
		ASSERT_TRUE(Durin::Asset::SavePackage(StaticMeshFixture->GetPackage()));
		ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		const Durin::Asset::FAssetCatalogEntry StaticMeshAssetData =
			Durin::Asset::FindAssetExact(StaticMeshFixturePath);
		ASSERT_NE(StaticMeshAssetData, nullptr);
		const Durin::Editor::FAssetThumbnailPackageFingerprint StaticMeshFingerprint = {
			.VirtualPath = StaticMeshAssetData->PackagePath,
			.AssetClassName = StaticMeshAssetData->AssetClassName,
			.PackageFormatVersion = StaticMeshAssetData->FormatVersion,
			.FileSize = static_cast<Durin::uint64>(StaticMeshAssetData->FileSize),
			.LastWriteTimeTicks = StaticMeshAssetData->LastWriteTimeTicks};
		const std::filesystem::path ThumbnailCacheRoot =
			Durin::Testing::GetTestWorkDirectory() / "StaticMeshRenderedCacheVulkan";
		Durin::Testing::RemoveTestWorkDirectory(ThumbnailCacheRoot);
		ASSERT_EQ(Durin::Mona::GetActiveUIBackend(), nullptr);
		Durin::Tests::FThumbnailTestUIBackend ThumbnailUIBackend;
		Durin::Tests::FScopedActiveUIBackend ThumbnailBackendScope(ThumbnailUIBackend);
		auto PumpCacheToReady = [&](Durin::Editor::FRenderedAssetThumbnailCache& Cache) {
			Durin::Editor::FAssetThumbnailView View;
			for (Durin::uint32 Attempt = 0; Attempt < 16; ++Attempt)
			{
				Cache.BeginFrame();
				Cache.Request(
					StaticMeshFingerprint,
					Durin::Editor::EAssetThumbnailPriority::Visible);
				View = Cache.Find(StaticMeshFixturePath);
				Cache.EndFrame();
				Durin::FlushRenderingCommands();
				if (View.State == Durin::Editor::EAssetThumbnailState::Ready
					&& View.Texture != nullptr)
					break;
			}
			return View;
		};
		{
			Durin::Editor::FRenderedAssetThumbnailCache Cache({}, {
				.CacheRoot = ThumbnailCacheRoot,
				.ObjectExtension = ".png"});
			const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(Cache);
			ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
				<< Ready.Diagnostic;
			ASSERT_NE(Ready.Texture, nullptr);
			EXPECT_TRUE(Ready.bHasTransparency);
			const Durin::Editor::FRenderedAssetThumbnailCacheStats Stats = Cache.GetStats();
			EXPECT_EQ(Stats.Pipeline.Loads, 1u);
			EXPECT_EQ(Stats.Pipeline.Renders, 1u);
			EXPECT_EQ(Stats.Pipeline.Readbacks, 1u);
			EXPECT_EQ(Stats.Pipeline.DiskHits, 0u);
			EXPECT_EQ(Stats.PreviewSceneCreations, 1u);
			EXPECT_EQ(Stats.PreviewSceneAssignments, 1u);
			EXPECT_EQ(Stats.UploadsCompleted, 1u);
			EXPECT_EQ(Stats.LiveGpuTextures, 1u);
			EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 1u);
			Cache.Clear();
			EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
			EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 0u);
		}
		{
			Durin::Editor::FRenderedAssetThumbnailCache WarmCache({}, {
				.CacheRoot = ThumbnailCacheRoot,
				.ObjectExtension = ".png"});
			const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(WarmCache);
			ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
				<< Ready.Diagnostic;
			const Durin::Editor::FRenderedAssetThumbnailCacheStats Stats = WarmCache.GetStats();
			EXPECT_EQ(Stats.Pipeline.DiskHits, 1u);
			EXPECT_EQ(Stats.Pipeline.Loads, 0u);
			EXPECT_EQ(Stats.Pipeline.Renders, 0u);
			EXPECT_EQ(Stats.Pipeline.Readbacks, 0u);
			EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
			EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
			EXPECT_EQ(Stats.UploadsCompleted, 1u);

			WarmCache.CancelPendingRequests();
			const Durin::Editor::FAssetThumbnailView Retained =
				WarmCache.Find(StaticMeshFixturePath);
			EXPECT_EQ(Retained.State, Durin::Editor::EAssetThumbnailState::Ready);
			EXPECT_EQ(Retained.Texture, Ready.Texture);
			WarmCache.BeginFrame();
			WarmCache.Request(
				StaticMeshFingerprint,
				Durin::Editor::EAssetThumbnailPriority::Visible);
			const Durin::Editor::FAssetThumbnailView Revisited =
				WarmCache.Find(StaticMeshFixturePath);
			WarmCache.EndFrame();
			EXPECT_EQ(Revisited.State, Durin::Editor::EAssetThumbnailState::Ready);
			EXPECT_EQ(Revisited.Texture, Ready.Texture);
			const Durin::Editor::FRenderedAssetThumbnailCacheStats RevisitedStats =
				WarmCache.GetStats();
			EXPECT_EQ(RevisitedStats.Pipeline.DiskHits, Stats.Pipeline.DiskHits);
			EXPECT_EQ(RevisitedStats.UploadsQueued, Stats.UploadsQueued);
			WarmCache.Clear();
		}

		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), nullptr));
		const std::vector<Durin::uint8> UntexturedPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), TextureResult.Asset));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.0, 0.0)));
		Durin::DStaticMesh* TriangleCaptureMesh = CaptureMesh;
		CaptureMesh = CaptureSphere;
		const std::vector<Durin::uint8> UV0Pixels = Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVChannel"), 3.0f));
		const std::vector<Durin::uint8> MissingUVFallbackPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(-1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(1.0, 0.0)));
		const std::vector<Durin::uint8> TransformedUVPixels =
			Capture(CaptureMaterial);
		EXPECT_EQ(UV0Pixels.size(), MissingUVFallbackPixels.size());
		EXPECT_EQ(TransformedUVPixels.size(), UV0Pixels.size());
		const Durin::FMaterialRenderBinding TransformedUVBinding =
			GetMaterialBinding(CaptureMaterial->GetRenderData());
		EXPECT_FLOAT_EQ(TransformedUVBinding.UVChannels[0], 3.0f);
		EXPECT_EQ(
			TransformedUVBinding.UVScales[0],
			Durin::FVector2f(-1.0f, 1.0f));
		EXPECT_EQ(
			TransformedUVBinding.UVOffsets[0],
			Durin::FVector2f(1.0f, 0.0f));

		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVChannel"), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(1.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.0, 0.0)));
		Durin::FMaterialSamplerState RepeatSampler;
		RepeatSampler.MinFilter = Durin::EMaterialSamplerMinFilter::Nearest;
		RepeatSampler.MagFilter = Durin::EMaterialSamplerMagFilter::Nearest;
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(RepeatSampler)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVRotation"), 1.57079633f));
		const std::vector<Durin::uint8> RotatedUVPixels = Capture(CaptureMaterial);
		EXPECT_NE(RotatedUVPixels, UV0Pixels);
		EXPECT_FLOAT_EQ(
			GetMaterialBinding(CaptureMaterial->GetRenderData()).UVRotations[0],
			1.57079633f);

		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorUVRotation"), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, 2.0)));
		ASSERT_TRUE(CaptureMaterial->SetVector2ParameterValue(
			Durin::FName("BaseColorUVOffset"), Durin::FVector2(0.75, 0.75)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(RepeatSampler)));
		const std::vector<Durin::uint8> RepeatPixels = Capture(CaptureMaterial);
		Durin::FMaterialSamplerState ClampSampler = RepeatSampler;
		ClampSampler.AddressU = Durin::EMaterialSamplerAddressMode::ClampToEdge;
		ClampSampler.AddressV = Durin::EMaterialSamplerAddressMode::ClampToEdge;
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::FName("BaseColorSamplerState"),
			Durin::EncodeMaterialSamplerState(ClampSampler)));
		const std::vector<Durin::uint8> ClampPixels = Capture(CaptureMaterial);
		EXPECT_NE(RepeatPixels, ClampPixels);
		EXPECT_EQ(
			GetMaterialBinding(CaptureMaterial->GetRenderData()).Samplers[0],
			ClampSampler);
		CaptureMesh = TriangleCaptureMesh;

		const std::array<const Durin::FName*, 8> TextureNames{
			&Durin::MaterialParameters::BaseColorTextureName(),
			&Durin::MaterialParameters::NormalTextureName(),
			&Durin::MaterialParameters::MetallicTextureName(),
			&Durin::MaterialParameters::RoughnessTextureName(),
			&Durin::MaterialParameters::AmbientOcclusionTextureName(),
			&Durin::MaterialParameters::EmissiveTextureName(),
			&Durin::MaterialParameters::OpacityTextureName(),
			&Durin::MaterialParameters::OpacityMaskTextureName()};
		const std::array<Durin::DTexture2D*, 8> RoleTextures{
			TextureResult.Asset,
			NormalTextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset,
			TextureResult.Asset,
			DataTextureResult.Asset,
			DataTextureResult.Asset};
		Durin::FMaterialRenderProxyRef TextureRoleProxy =
			CaptureMaterial->GetMaterialRenderProxy();
		for (size_t Role = 0; Role < TextureNames.size(); ++Role)
		{
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureNames[Role], RoleTextures[Role]));
			Durin::FMaterialRenderData ProxyRenderData;
			struct FCaptureTextureRoleProxyCommand
			{
				static constexpr auto GetName() -> const char*
				{
					return "CaptureTextureRoleProxy";
				}
			};
			Durin::EnqueueRenderCommand<FCaptureTextureRoleProxyCommand>(
				[TextureRoleProxy, &ProxyRenderData](
					Durin::FRHICommandListImmediate&) {
					ProxyRenderData =
						TextureRoleProxy->Resolve_RenderThread();
				});
			WaitForRenderingThread();
			const Durin::FMaterialRenderData DirectRenderData =
				CaptureMaterial->GetRenderData();
			EXPECT_EQ(
				ProxyRenderData.PipelineIdentity,
				DirectRenderData.PipelineIdentity);
			EXPECT_TRUE(std::ranges::equal(
				ProxyRenderData.Representation.GetUniformPayload(),
				DirectRenderData.Representation.GetUniformPayload()));
			EXPECT_TRUE(std::ranges::equal(
				ProxyRenderData.Representation.GetResources(),
				DirectRenderData.Representation.GetResources()));
			const Durin::FMaterialRenderBinding RoleBinding =
				GetMaterialBinding(ProxyRenderData);
			EXPECT_EQ(
				RoleBinding.Textures[Role].GetReference(),
				RoleTextures[Role]->GetTextureReferenceRHI().GetReference());
			for (size_t OtherRole = 0;
				OtherRole < RoleBinding.Textures.size(); ++OtherRole)
			{
				if (OtherRole != Role)
				{
					EXPECT_EQ(RoleBinding.Textures[OtherRole], nullptr);
				}
			}
			ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
				*TextureNames[Role], nullptr));
		}
		Durin::ReleaseMaterialRenderProxy_GameThread(
			std::move(TextureRoleProxy));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.55, 0.45, 0.35)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.5f));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.0, 0.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.0)));
		const std::vector<Durin::uint8> PbrBaselinePixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 1.0f));
		const std::vector<Durin::uint8> MetallicOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::MetallicName(), 0.0f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.1f));
		const std::vector<Durin::uint8> RoughnessOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::RoughnessName(), 0.5f));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.6, 0.0, 0.8)));
		const std::vector<Durin::uint8> NormalOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::NormalName(),
			Durin::FVector3(0.0, 0.0, 1.0)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.15, 0.05, 0.0)));
		const std::vector<Durin::uint8> EmissiveOnlyPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.15, 0.7, 0.2)));
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::EmissiveName(),
			Durin::FVector3(0.1, 0.05, 0.0)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		const Durin::FMaterialPipelineIdentity LitPipelineIdentity =
			CaptureMaterial->GetRenderData().PipelineIdentity;
		const std::vector<Durin::uint8> LitEmissivePixels =
			Capture(CaptureMaterial);
		Durin::FMaterialStaticProperties StaticProperties;
		StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		EXPECT_NE(
			CaptureMaterial->GetRenderData().PipelineIdentity,
			LitPipelineIdentity);
		const std::vector<Durin::uint8> StaticIdentityPixels =
			Capture(CaptureMaterial);
		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
		StaticProperties.OpacityMaskThreshold = 0.4f;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.39f));
		const std::vector<Durin::uint8> MaskedBelowPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.4f));
		const std::vector<Durin::uint8> MaskedEqualPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 0.41f));
		const std::vector<Durin::uint8> MaskedAbovePixels =
			Capture(CaptureMaterial);

		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.0f));
		const std::vector<Durin::uint8> TranslucentZeroPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		const std::vector<Durin::uint8> TranslucentPartialPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 1.0f));
		const std::vector<Durin::uint8> TranslucentFullPixels =
			Capture(CaptureMaterial);

		StaticProperties.BlendMode = Durin::EMaterialBlendMode::Opaque;
		ASSERT_TRUE(CaptureMaterial->SetStaticProperties(StaticProperties));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityName(), 0.4f));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::OpacityMaskName(), 1.0f));
		const Durin::FConsoleCommandResult ReloadResult =
			Durin::FConsoleCommandRegistry::Get().Execute(
				"renderer.reload-shaders all");
		ASSERT_TRUE(ReloadResult.bSuccess) << ReloadResult.Message;
		const std::vector<Durin::uint8> ReloadedPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/TC_Preview", CaptureCubePath));
		const Durin::Asset::Forge::FTextureCubeImportResult CubeResult =
			Durin::Asset::Forge::ImportTextureCubeFaces(
				Durin::Tests::GetRenderedThumbnailDirectionalCubeFaces(),
				CaptureCubePath.ToString());
		ASSERT_TRUE(CubeResult) << CubeResult.Message;
		CaptureCube = CubeResult.Asset;
		CaptureCubeReference = CaptureCube->GetTextureReferenceRHI();
		Durin::FlushRenderingCommands();

		Durin::FRHITextureReferenceRef Texture2DReference =
			TextureResult.Asset->GetTextureReferenceRHI();
		const Durin::FViewEnvironmentOverride CubeEnvironment{
			.TextureReference = CaptureCubeReference};
		const Durin::FViewEnvironmentOverride Texture2DEnvironment{
			.TextureReference = Texture2DReference};
		const Durin::uint32 CubeReferenceBaseline =
			CaptureCubeReference->GetRefCount();
		const Durin::uint32 Texture2DReferenceBaseline =
			Texture2DReference->GetRefCount();
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.SetView(Error)) << Error;
		EXPECT_EQ(
			CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u);
		ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline + 1u);
		Pool.Reset();
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline);
		Pool.Reset();
		EXPECT_EQ(
			Texture2DReference->GetRefCount(), Texture2DReferenceBaseline);
		ASSERT_TRUE(Pool.SetView(Error)) << Error;
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		EXPECT_EQ(
			CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u);
		Pool.Reset();
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> DirectEnvironmentPixels;
		ASSERT_EQ(
			Pool.PollCapture(DirectEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		ASSERT_FALSE(DirectEnvironmentPixels.empty());
		Pool.Reset();

		Durin::FTextureRHIRef OriginalCubeTarget;
		struct FRetargetRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "RetargetRenderedThumbnailEnvironment";
			}
		};
		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		Durin::EnqueueRenderCommand<FRetargetRenderedThumbnailEnvironment>(
			[Reference = CaptureCubeReference, &OriginalCubeTarget](
				Durin::FRHICommandListImmediate&) {
				OriginalCubeTarget =
					Reference->GetReferencedTexture_RenderThread();
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(),
					Durin::GetDefaultCubeTexture_RenderThread());
			});
		Durin::FlushRenderingCommands();
		ASSERT_NE(OriginalCubeTarget, nullptr);
		EXPECT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> RetargetedEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(RetargetedEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		EXPECT_NE(RetargetedEnvironmentPixels, DirectEnvironmentPixels);
		Pool.Reset();

		struct FRestoreRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "RestoreRenderedThumbnailEnvironment";
			}
		};
		auto RestoreCubeTarget = [&] {
			Durin::EnqueueRenderCommand<FRestoreRenderedThumbnailEnvironment>(
				[Reference = CaptureCubeReference, OriginalCubeTarget](
					Durin::FRHICommandListImmediate&) {
					Durin::GDynamicRHI->RHIUpdateTextureReference(
						Reference.GetReference(), OriginalCubeTarget.GetReference());
				});
			Durin::FlushRenderingCommands();
		};
		RestoreCubeTarget();

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		struct FClearRenderedThumbnailEnvironment
		{
			static constexpr auto GetName() -> const char*
			{
				return "ClearRenderedThumbnailEnvironment";
			}
		};
		Durin::EnqueueRenderCommand<FClearRenderedThumbnailEnvironment>(
			[Reference = CaptureCubeReference](
				Durin::FRHICommandListImmediate&) {
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(), nullptr);
			});
		Durin::FlushRenderingCommands();
		EXPECT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> UnavailableEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(UnavailableEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Failed);
		EXPECT_TRUE(UnavailableEnvironmentPixels.empty());
		EXPECT_NE(Error.find("view environment"), std::string::npos);
		Pool.Reset();
		RestoreCubeTarget();

		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> EmptyScenePixels;
		ASSERT_EQ(
			Pool.PollCapture(EmptyScenePixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		EXPECT_EQ(EmptyScenePixels.size(), DirectEnvironmentPixels.size());
		EXPECT_NE(EmptyScenePixels, DirectEnvironmentPixels);
		Pool.Reset();

		ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> FailedEnvironmentPixels;
		EXPECT_EQ(
			Pool.PollCapture(FailedEnvironmentPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Failed);
		EXPECT_TRUE(FailedEnvironmentPixels.empty());
		EXPECT_NE(Error.find("view environment"), std::string::npos);
		Pool.Reset();

		ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
		struct FRenderedThumbnailCancellationGate
		{
			std::mutex Mutex;
			std::condition_variable Condition;
			bool bEntered = false;
			bool bReleased = false;
		};
		struct FBlockRenderedThumbnailCaptureForCancellation
		{
			static constexpr auto GetName() -> const char*
			{
				return "BlockRenderedThumbnailCaptureForCancellation";
			}
		};
		const auto Gate = std::make_shared<FRenderedThumbnailCancellationGate>();
		Durin::EnqueueRenderCommand<FBlockRenderedThumbnailCaptureForCancellation>(
			[Gate](Durin::FRHICommandListImmediate&) {
				std::unique_lock Lock(Gate->Mutex);
				Gate->bEntered = true;
				Gate->Condition.notify_all();
				Gate->Condition.wait(Lock, [&Gate] { return Gate->bReleased; });
			});
		{
			std::unique_lock Lock(Gate->Mutex);
			Gate->Condition.wait(Lock, [&Gate] { return Gate->bEntered; });
		}
		const bool bCancelledCaptureStarted = Pool.BeginCapture(Error);
		Pool.Reset();
		const Durin::uint32 QueuedReferenceCount =
			CaptureCubeReference->GetRefCount();
		{
			std::lock_guard Lock(Gate->Mutex);
			Gate->bReleased = true;
		}
		Gate->Condition.notify_all();
		Durin::FlushRenderingCommands();
		EXPECT_TRUE(bCancelledCaptureStarted) << Error;
		EXPECT_GT(QueuedReferenceCount, CubeReferenceBaseline);
		EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
		std::vector<Durin::uint8> CancelledPixels;
		EXPECT_EQ(
			Pool.PollCapture(CancelledPixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Idle);
		EXPECT_TRUE(CancelledPixels.empty());
		EXPECT_TRUE(Error.empty());

		std::vector<Durin::uint8> CubePixels;
		ASSERT_TRUE(Pool.SetTextureCube(CubeResult.Asset, Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			Pool.PollCapture(CubePixels, Error),
			Durin::Editor::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		Pool.Reset();
		ASSERT_EQ(MaterialPixels.size(), 64u * 64u * 4u);
		EXPECT_EQ(TwoSidedBackPixels, TwoSidedFrontPixels);
		ASSERT_EQ(InstancePixels.size(), MaterialPixels.size());
		ASSERT_EQ(UntexturedPixels.size(), MaterialPixels.size());
		const size_t Corner = 0;
		const size_t Center = (32u * 64u + 32u) * 4u;
		EXPECT_EQ(MaterialPixels[Corner + 3], 0u);
		EXPECT_GT(MaterialPixels[Center + 3], 0u);
		EXPECT_EQ(CubePixels[Corner + 3], 255u);
		const std::array CornerRgb = {
			MaterialPixels[Corner], MaterialPixels[Corner + 1], MaterialPixels[Corner + 2]};
		const std::array MaterialCenterRgb = {
			MaterialPixels[Center], MaterialPixels[Center + 1], MaterialPixels[Center + 2]};
		const std::array InstanceCenterRgb = {
			InstancePixels[Center], InstancePixels[Center + 1], InstancePixels[Center + 2]};
		EXPECT_NE(CornerRgb, MaterialCenterRgb);
		EXPECT_NE(MaterialCenterRgb, InstanceCenterRgb);
		EXPECT_NE(InheritedBeforePixels, InheritedAfterPixels);
		EXPECT_NE(MaterialPixels, UntexturedPixels);
		EXPECT_NE(PbrBaselinePixels, MetallicOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, RoughnessOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, NormalOnlyPixels);
		EXPECT_NE(PbrBaselinePixels, EmissiveOnlyPixels);
		EXPECT_NE(LitEmissivePixels, StaticIdentityPixels);
		EXPECT_EQ(StaticIdentityPixels, ReloadedPixels);
		EXPECT_NEAR(
			static_cast<int>(StaticIdentityPixels[Center + 2]),
			static_cast<int>(MapSrgbChannelThroughDisplay(124u)), 2);
		EXPECT_NEAR(static_cast<int>(StaticIdentityPixels[Center + 3]), 102, 2);
		EXPECT_EQ(MaskedBelowPixels[Center + 3], 0u);
		EXPECT_GT(MaskedEqualPixels[Center + 3], 0u);
		EXPECT_EQ(MaskedEqualPixels, MaskedAbovePixels);
		EXPECT_EQ(TranslucentZeroPixels[Center + 3], 0u);
		EXPECT_NEAR(
			static_cast<int>(TranslucentPartialPixels[Center + 3]), 102, 2);
		EXPECT_EQ(TranslucentFullPixels[Center + 3], 255u);
		EXPECT_LT(
			TranslucentPartialPixels[Center], TranslucentFullPixels[Center]);
		ASSERT_EQ(CubePixels.size(), MaterialPixels.size());
		const std::array CubeCenterRgb = {
			CubePixels[Center], CubePixels[Center + 1], CubePixels[Center + 2]};
		EXPECT_NE(CubeCenterRgb, CornerRgb);
		EXPECT_NE(CubeCenterRgb, (std::array<Durin::uint8, 3>{0, 0, 0}));
		std::unordered_set<Durin::uint32> CubeCornerColors;
		for (const size_t CornerPixel : std::array<size_t, 4>{
				0,
				(64u - 1u) * 4u,
				((64u - 1u) * 64u) * 4u,
				(64u * 64u - 1u) * 4u})
		{
			CubeCornerColors.insert(
				static_cast<Durin::uint32>(CubePixels[CornerPixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[CornerPixel + 1]) << 8
				| CubePixels[CornerPixel + 2]);
		}
		EXPECT_GT(CubeCornerColors.size(), 1u);
		std::unordered_set<Durin::uint32> CubeColors;
		for (size_t Pixel = 0; Pixel < CubePixels.size(); Pixel += 4)
		{
			CubeColors.insert(
				static_cast<Durin::uint32>(CubePixels[Pixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[Pixel + 1]) << 8
				| CubePixels[Pixel + 2]);
		}
		EXPECT_GT(CubeColors.size(), 8u);

		struct FEndRenderedThumbnailFrame
		{
			static constexpr auto GetName() -> const char* { return "EndRenderedThumbnailFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndRenderedThumbnailFrame>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}

	Durin::GEngine = nullptr;
	ASSERT_NE(CaptureMesh, nullptr);
	ASSERT_NE(CaptureSphere, nullptr);
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(CaptureTexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		DataTexturePath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(DataTexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		NormalTexturePath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(NormalTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(CaptureCubePath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(StaticMeshFixturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(StaticMeshMaterialPath));
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(CaptureCube);
	Durin::MarkAsGarbage(InheritedInstance);
	Durin::MarkAsGarbage(CaptureInstance);
	Durin::MarkAsGarbage(CaptureMaterial);
	Durin::MarkAsGarbage(CaptureMesh);
	Durin::MarkAsGarbage(LowRoughnessMaterial);
	Durin::MarkAsGarbage(LowRoughnessMesh);
	PreloadedSphere = {};
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SpherePath));
	Durin::CollectGarbage();
	struct FRetireRenderedThumbnailCubeResource
	{
		static constexpr auto GetName() -> const char*
		{
			return "RetireRenderedThumbnailCubeResource";
		}
	};
	Durin::EnqueueRenderCommand<FRetireRenderedThumbnailCubeResource>(
		[Reference = std::move(CaptureCubeReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	RendererLifecycle.Shutdown();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	// The native suite may create another RHI in the same process; force the
	// process-wide immediate list to acquire that device's context next time.
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
}
#include "TextureAuthoringTestEnvironment.h"
