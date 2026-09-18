#include "Asset/AssetCompilingManager.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "EditorReimportHandler.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/VolumeTextureImportData.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "EncodedSourceSnapshot.h"
#include "Texture/TextureCube.h"
#include "TextureTestSupport.h"
#include "Texture/VolumeTextureFactoryTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto InitializeSingleAssetImportTests() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SingleAssetImportStage2";
		static const bool Initialized = [&] {
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::Testing::RegisterMountPointForTests(
				"/SingleAssetStage2/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
		return Root;
	}

}

TEST(FAssetForgeBuiltinsImageSourcePolicyTests, KeepsCodecCapabilitySeparateFromAssetAdmission)
{
	using namespace Durin::AssetForge::Builtins;
	EXPECT_TRUE(IsTexture2DSourceExtension(".PNG"));
	EXPECT_FALSE(IsTexture2DSourceExtension(".hdr"));
	EXPECT_TRUE(IsTextureCubeFaceSourceExtension(".tga"));
	EXPECT_FALSE(IsTextureCubeFaceSourceExtension(".hdr"));
	EXPECT_TRUE(IsTextureCubePanoramaSourceExtension(".hdr"));
	EXPECT_FALSE(IsTextureCubePanoramaSourceExtension(".gif"));
	EXPECT_TRUE(IsSceneSurfaceImageEncodingSupported(EImportedImageEncoding::Png));
	EXPECT_FALSE(IsSceneSurfaceImageEncodingSupported(
		static_cast<EImportedImageEncoding>(255)));
}

TEST(FSingleAssetImportTests, ReimportsGeometryDirectlyFromFamilyImportData)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Imported = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/SingleAssetStage2/Geometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Imported.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	const Durin::FSourceFile* ImportedSource =
		ImportData->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_FALSE(ImportedSource->Hint.empty());
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Imported.Asset, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Imported.Asset);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
	ASSERT_NE(Imported.Asset->GetAssetImportData(), nullptr);
	EXPECT_NE(Imported.Asset->GetAssetImportData()->GetSourceData().FindByRole("source"), nullptr);
}

TEST(FSingleAssetImportTests, FailedFamilyFactoriesDiscardTheirFormalPackages)
{
	const std::filesystem::path Root = InitializeSingleAssetImportTests();
	const std::array<uint8, 4> InvalidBytes{1, 2, 3, 4};
	const std::filesystem::path VolumeSource = Root / "Invalid.png";
	const std::filesystem::path MeshSource = Root / "Invalid.obj";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{InvalidBytes}), VolumeSource));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{InvalidBytes}), MeshSource));

	const auto Volume = Durin::AssetForge::Builtins::ImportVolumeTextureForTest(
		VolumeSource.generic_string(), "/SingleAssetStage2/InvalidVolume", {
			.SliceWidth = 1, .SliceHeight = 1, .Depth = 1,
			.TilesX = 1, .TilesY = 1});
	const auto Mesh = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		MeshSource.generic_string(), "/SingleAssetStage2/InvalidMesh");
	EXPECT_FALSE(Volume);
	EXPECT_FALSE(Mesh);

	for (const std::string_view PathText : {
		"/SingleAssetStage2/InvalidVolume",
		"/SingleAssetStage2/InvalidMesh"})
	{
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(PathText, Path));
		EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
	}
}

TEST(FSingleAssetImportTests, ReimportsPanoramaTextureCubeFromCapturedBytes)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTextureCubeDdc");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "EquirectangularPanorama" / "AnalyticalLDR.tga";
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Imported = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Source.generic_string(), "/SingleAssetStage2/Panorama");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = Imported.Asset->GetAssetImportData();
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	ASSERT_NE(ImportData->GetSourceData().FindByRole("panorama"), nullptr);
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Imported.Asset, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_NE(Imported.Asset->GetPlatformData(), nullptr);
}

TEST(FSingleAssetImportTests, DerivedStateValidationChecksBaseBeforeEmptyStateShortcuts)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	FStaticMeshImportDataState Mesh;
	FVolumeTextureImportDataState Volume;
	ASSERT_TRUE(Mesh.Validate());
	ASSERT_TRUE(Volume.Validate());
	++Mesh.SchemaVersion;
	++Volume.SchemaVersion;
	EXPECT_EQ(Mesh.Validate().Error.Code, EAssetImportDataError::UnsupportedSchema);
	EXPECT_EQ(Volume.Validate().Error.Code, EAssetImportDataError::UnsupportedSchema);
	Volume.SchemaVersion = AssetImportDataSchemaVersion;
	Volume.Depth = 1;
	const auto Validation = Volume.Validate();
	EXPECT_EQ(Validation.Error.Code, EAssetImportDataError::ModuleRejected);
}

TEST(FSingleAssetImportTests, DerivedValidationRejectsInvalidAxisAtlasAndRole)
{
	InitializeSingleAssetImportTests();
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const FSourceFile Source{.Role = "source", .ContentHashLow = 1, .ContentHashHigh = 2, .ByteCount = 1};
	FStaticMeshImportDataState Mesh;
	Mesh.SourceData.Sources = {Source};
	Mesh.ImportSettings.RightAxis = EStaticMeshImportAxis::PositiveX;
	const auto InvalidMesh = Mesh.Validate();
	EXPECT_EQ(InvalidMesh.Error.Code, EAssetImportDataError::ModuleRejected);
	Mesh.ImportSettings = {};
	ASSERT_TRUE(Mesh.Validate());
	auto* MeshData = NewObject<DStaticMeshImportData>(nullptr, "TypedMeshImportData");
	ASSERT_NE(MeshData, nullptr);
	MeshData->SetState(Mesh);
	const DAssetImportData& Base = *MeshData;
	EXPECT_TRUE(Base.Validate());

	FVolumeTextureImportDataState Volume;
	Volume.SourceData.Sources = {Source};
	Volume.SliceWidth = 4;
	Volume.SliceHeight = 8;
	Volume.Depth = 5;
	Volume.TilesX = 2;
	Volume.TilesY = 2;
	const auto InvalidAtlas = Volume.Validate();
	EXPECT_EQ(InvalidAtlas.Error.Code, EAssetImportDataError::ModuleRejected);
	Volume.Depth = 4;
	ASSERT_TRUE(Volume.Validate());
	Volume.SourceData.Sources[0].Role = "other";
	const auto InvalidRole = Volume.Validate();
	EXPECT_EQ(InvalidRole.Error.Code, EAssetImportDataError::ModuleRejected);
	Volume.SourceData.Sources.clear();
}

TEST(FSingleAssetImportTests, FactorySourceAdmissionRetainsTypedCauseAfterDiscard)
{
	const auto Root = InitializeSingleAssetImportTests();
	const auto Missing = Root / "MissingSource.obj";
	const auto Unsupported = Root / "UnsupportedSource.txt";
	const std::array<uint8, 1> Bytes{0};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span{Bytes}), Unsupported));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SingleAssetStage2/SourceRejection", Path));
	for (const auto& Source : {Missing, Unsupported})
	{
		auto* Factory = Durin::NewObject<Durin::AssetForge::Builtins::DStaticMeshFactory>(
			nullptr, "SourceRejectionFactory", Durin::EObjectFlags::Transient);
		const auto Result = Durin::IAssetTools::Get().ImportPackageLeafAssetForTesting(
			Path, Durin::DStaticMesh::StaticClass(), Source.generic_string(), Factory);
		EXPECT_FALSE(Result);
		EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
		ASSERT_TRUE(Result.FactoryCause) << Result.Message;
		ASSERT_EQ(Result.FactoryCause->GetEntries().size(), 1u);
		const auto& Failure = Result.FactoryCause->GetEntries()[0].Failure;
		ASSERT_TRUE(Failure);
		EXPECT_EQ(Failure->Code, Source == Missing
			? Durin::EFactoryError::SourceMissing : Durin::EFactoryError::SourceFormat);
		EXPECT_EQ(Failure->Filename, Source.generic_string());
	}
}

TEST(FSingleAssetImportTests, FactoryRetainsSettingsAxesAfterDiscard)
{
	InitializeSingleAssetImportTests();
	const auto Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SingleAssetStage2/SettingsRejection", Path));
	auto* Factory = Durin::NewObject<Durin::AssetForge::Builtins::DStaticMeshFactory>(
		nullptr, "SettingsRejectionFactory", Durin::EObjectFlags::Transient);
	Durin::FStaticMeshImportSettings Settings;
	Settings.RightAxis = Settings.ForwardAxis;
	Factory->SetImportSettings(Settings);
	const auto Result = Durin::IAssetTools::Get().ImportPackageLeafAssetForTesting(
		Path, Durin::DStaticMesh::StaticClass(), Source.generic_string(), Factory);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
	ASSERT_TRUE(Result.FactoryCause) << Result.Message;
	ASSERT_EQ(Result.FactoryCause->GetEntries().size(), 1u);
	const auto& Failure = Result.FactoryCause->GetEntries()[0].Failure;
	ASSERT_TRUE(Failure);
	EXPECT_EQ(Failure->Code, Durin::EFactoryError::StaticMeshSettings);
	ASSERT_TRUE(Failure->StaticMeshSettingsCause);
	EXPECT_EQ(Failure->StaticMeshSettingsCause->Code, Durin::EStaticMeshImportSettingsError::RepeatedAxis);
	EXPECT_EQ(Failure->StaticMeshSettingsCause->ForwardAxis, Settings.ForwardAxis);
	EXPECT_EQ(Failure->StaticMeshSettingsCause->RightAxis, Settings.RightAxis);
	EXPECT_EQ(Failure->Filename, Source.generic_string());
}

TEST(FSingleAssetImportTests, VolumeFactoryRetainsModuleCauseAfterPackageDiscard)
{
	const auto Root = InitializeSingleAssetImportTests();
	const auto Source = Root / "InvalidDomainSource.png";
	const std::array<std::byte, 2> Bytes{std::byte{1}, std::byte{2}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, Source));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SingleAssetStage2/DomainFailure", Path));
	for (const bool InvalidSettings : {true, false})
	{
		auto* Factory = Durin::NewObject<Durin::AssetForge::Builtins::DVolumeTextureFactory>(
			nullptr, "DomainFailureFactory", Durin::EObjectFlags::Transient);
		Factory->SetImportSettings({.SliceWidth = InvalidSettings ? 0u : 1u,
			.SliceHeight = 1, .Depth = 1, .TilesX = 1, .TilesY = 1});
		const auto Result = Durin::IAssetTools::Get().ImportPackageLeafAssetForTesting(
			Path, Durin::DVolumeTexture::StaticClass(), Source.generic_string(), Factory);
		EXPECT_FALSE(Result);
		EXPECT_EQ(Durin::FindResidentPackage(Path), nullptr);
		ASSERT_TRUE(Result.FactoryCause) << Result.Message;
		ASSERT_EQ(Result.FactoryCause->GetEntries().size(), 1u);
		const auto& Entry = Result.FactoryCause->GetEntries()[0];
		EXPECT_TRUE(Entry.Message.empty());
		const auto* Detail = dynamic_cast<const Durin::AssetForge::Builtins::FVolumeTextureFactoryError*>(Entry.DomainFailure.get());
		ASSERT_NE(Detail, nullptr);
		using namespace Durin::AssetForge::Builtins;
		if (InvalidSettings)
		{
			const auto* Cause = std::get_if<FVolumeTextureImportSettingsError>(&Detail->Cause);
			ASSERT_NE(Cause, nullptr);
			EXPECT_EQ(Cause->Code, EVolumeTextureImportSettingsError::Dimensions);
			EXPECT_EQ(Cause->Settings.SliceWidth, 0u);
		}
		else
		{
			const auto* Cause = std::get_if<FVolumeTextureRebuildError>(&Detail->Cause);
			ASSERT_NE(Cause, nullptr);
			EXPECT_EQ(Cause->Code, EVolumeTextureRebuildError::Translation);
			ASSERT_TRUE(Cause->TranslationCause);
			EXPECT_EQ(Cause->TranslationCause->Code, EVolumeTextureTranslationError::Signature);
			EXPECT_EQ(Cause->TranslationCause->Filename, Source.filename().generic_string());
		}
	}
}

TEST(FSingleAssetImportTests, TexturePreparationRetainsCaptureAndTranslationCauses)
{
	const auto Root = InitializeSingleAssetImportTests();
	using namespace Durin::AssetForge::Builtins;
	FPreparedTexture2DImport Prepared;
	Prepared.Filename = "previous";
	const auto MissingPath = Root / "MissingPreparation.png";
	const auto Missing = PrepareTexture2DImport(MissingPath.generic_string(), Prepared);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error.Code, ETexture2DPreparationError::Capture);
	ASSERT_TRUE(Missing.Error.CaptureCause);
	EXPECT_EQ(Missing.Error.CaptureCause->Code, EEncodedSourceError::FileSize);
	EXPECT_EQ(Missing.Error.CaptureCause->PhysicalPath, MissingPath);
	EXPECT_TRUE(Prepared.Filename.empty());
	EXPECT_FALSE(Prepared.Source.IsValid());
	const auto InvalidPath = Root / "InvalidPreparation.png";
	const std::array<std::byte, 2> Bytes{std::byte{1}, std::byte{2}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Bytes, InvalidPath));
	const auto Invalid = PrepareTexture2DImport(InvalidPath.generic_string(), Prepared);
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.Error.Code, ETexture2DPreparationError::Translation);
	ASSERT_TRUE(Invalid.Error.TranslationCause);
	ASSERT_TRUE(Invalid.Error.TranslationCause->DecodeCause);
	EXPECT_EQ(Invalid.Error.TranslationCause->DecodeCause->Code, Durin::Image::EImageDecodeError::InvalidImage);
	EXPECT_EQ(Invalid.Error.Filename, InvalidPath.generic_string());
	EXPECT_EQ(Missing.Error.CaptureCause->PhysicalPath, MissingPath);
	EXPECT_TRUE(Prepared.Filename.empty());
}
