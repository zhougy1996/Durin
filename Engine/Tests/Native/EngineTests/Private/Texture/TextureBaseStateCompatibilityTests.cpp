#include "TextureTestSupport.h"

#include "Asset/PackageInspection.h"
#include "Asset/Load.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace
{
	constexpr std::string_view FixtureDirectory = "TextureBaseStateCompatibility";

	auto CopyFixtureTree(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Source =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / FixtureDirectory / Name;
		const std::filesystem::path Destination =
			Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Destination);
		std::filesystem::copy(Source, Destination,
			std::filesystem::copy_options::recursive
				| std::filesystem::copy_options::overwrite_existing);
		return Destination;
	}

	auto InspectFixture(const std::filesystem::path& Path,
		std::string_view PackageIdentity, std::string_view SourceOwner,
		bool bExpectAuthoredState) -> void
	{
		Durin::FPackagePath PackagePath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(PackageIdentity, PackagePath)
			|| Durin::FPackagePath::TryCreateProjectContent(PackageIdentity, PackagePath));
		Durin::FAssetPackageInspection Inspection;
		ASSERT_TRUE(Durin::InspectAssetPackage(
			Path.generic_string(), PackagePath, Inspection));
		const Durin::FAssetPackageField* Source = Inspection.FindField("Source");
		EXPECT_EQ(Source != nullptr, bExpectAuthoredState);
		if (Source) EXPECT_EQ(Source->DeclaringClass, SourceOwner);
		const Durin::FAssetPackageField* ImportData = Inspection.FindField("AssetImportData");
		EXPECT_EQ(ImportData != nullptr, bExpectAuthoredState);
		if (ImportData) EXPECT_EQ(ImportData->DeclaringClass, SourceOwner);
	}

	auto ExpectImportData(const Durin::DTexture& Texture,
		std::string_view Hint, uint64 HashSeed) -> void
	{
		const Durin::DAssetImportData* ImportData = Texture.GetAssetImportData();
		ASSERT_NE(ImportData, nullptr);
		EXPECT_EQ(ImportData->GetOuter(), &Texture);
		const Durin::FSourceFile* Source =
			ImportData->GetSourceData().FindByRole("source");
		ASSERT_NE(Source, nullptr);
		EXPECT_EQ(Source->Hint, Hint);
		EXPECT_EQ(Source->ContentHashLow, HashSeed);
		EXPECT_EQ(Source->ContentHashHigh, HashSeed + 1);
	}

	auto ExpectCanonicalOverride(const Durin::DTexture& Texture,
		std::string_view FieldName) -> void
	{
		const auto Entries = Texture.GetAuthoredOverrideEntries();
		const auto It = std::ranges::find_if(Entries, [&](const auto& Entry) {
			return !Entry.Path.empty()
				&& Entry.Path.front().Kind == Durin::EAuthoredOverridePathTokenKind::Field
				&& Entry.Path.front().DeclaringType == Durin::FName("Durin::DTexture")
				&& Entry.Path.front().FieldName == Durin::FName(FieldName);
		});
		EXPECT_NE(It, Entries.end());
	}

	auto ExpectPropertyMoveEvidence(const Durin::FAssetLoadReport& Report,
		std::string_view StoredOwner, std::string_view FieldName) -> void
	{
		const std::string Stored = std::format("{}::{}", StoredOwner, FieldName);
		const std::string Current = std::format("Durin::DTexture::{}", FieldName);
		EXPECT_TRUE(std::ranges::any_of(Report.CanonicalizationEvidence,
			[&](const auto& Evidence) {
				return Evidence.Kind == Durin::EAssetReflectedIdentityKind::Property
					&& Evidence.StoredIdentity == Stored
					&& Evidence.CurrentIdentity == Current;
			}));
	}

	template<class T>
	auto ExpectLazyCookedInstall(T& Texture) -> void
	{
		const Durin::DTexture& BaseTexture = Texture;
		const Durin::EBulkDataState BulkState =
			BaseTexture.GetCookedPlatformData().GetState();
		const uint64 Revision = BaseTexture.GetBuildRevision();
		EXPECT_EQ(Texture.GetPlatformData(), nullptr);
		EXPECT_FALSE(BaseTexture.HasPlatformData());
		EXPECT_EQ(BaseTexture.GetCookedPlatformData().GetState(), BulkState);
		EXPECT_EQ(BaseTexture.GetBuildRevision(), Revision);
		ASSERT_TRUE(Texture.EnsurePlatformDataLoadedBlocking());
		ASSERT_NE(Texture.GetPlatformData(), nullptr);
		const auto* Installed = Texture.GetPlatformData();
		const uint64 InstalledRevision = Texture.GetBuildRevision();
		ASSERT_TRUE(Texture.EnsurePlatformDataLoadedBlocking());
		EXPECT_EQ(Texture.GetPlatformData(), Installed);
		EXPECT_EQ(Texture.GetBuildRevision(), InstalledRevision);
	}
}

TEST(FTextureBaseStateCompatibilityTests, PreMoveAuthoredFixturesPreserveFamilyState)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	const std::filesystem::path Root = CopyFixtureTree("Authored");
	Durin::Testing::RegisterMountPointForTests(
		"/TextureLegacyFixtures/", Root.generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(Durin::EAssetRegistryScanMode::FullValidation));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "Texture2D.dasset",
		"/TextureLegacyFixtures/Texture2D", "Durin::DTexture2D", true));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "TextureCube.dasset",
		"/TextureLegacyFixtures/TextureCube", "Durin::DTextureCube", true));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "VolumeTexture.dasset",
		"/TextureLegacyFixtures/VolumeTexture", "Durin::DVolumeTexture", true));

	Durin::FPackagePath Texture2DPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureLegacyFixtures/Texture2D", Texture2DPath));
	Durin::DTexture2D* Texture2D = nullptr;
	Durin::FAssetLoadReport Texture2DReport;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(Texture2DPath),
		Texture2D, &Texture2DReport));
	ASSERT_NE(Texture2D, nullptr);
	ExpectPropertyMoveEvidence(
		Texture2DReport, "Durin::DTexture2D", "Source");
	ExpectPropertyMoveEvidence(
		Texture2DReport, "Durin::DTexture2D", "AssetImportData");
	EXPECT_TRUE(Texture2D->GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_FALSE(Texture2D->GetPackage()->IsDirty());
	EXPECT_EQ(Texture2D->GetSource().Width, 2u);
	EXPECT_EQ(Texture2D->GetSource().Height, 1u);
	EXPECT_TRUE(Texture2D->GetSource().bHasTransparency);
	EXPECT_EQ(Texture2D->GetUsage(), Durin::ETextureUsage::DataMask);
	EXPECT_FALSE(Texture2D->IsSRGB());
	EXPECT_EQ(Texture2D->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ExpectImportData(*Texture2D, "Texture2D.source", 101);
	ExpectCanonicalOverride(*Texture2D, "Source");
	ExpectCanonicalOverride(*Texture2D, "AssetImportData");
	const Durin::FXxHash128 Texture2DSourceId =
		Texture2D->GetSource().Payload.GetPayloadId();
	Durin::FPackagePath DuplicatePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureLegacyFixtures/Texture2DLegacyDuplicate", DuplicatePath));
	Durin::DPackage* DuplicateOuter = Durin::CreatePackage(DuplicatePath);
	ASSERT_NE(DuplicateOuter, nullptr);
	auto* Texture2DDuplicate = Durin::Cast<Durin::DTexture2D>(
		Durin::DuplicateObject(Texture2D, DuplicateOuter, "Texture2DLegacyDuplicate"));
	ASSERT_NE(Texture2DDuplicate, nullptr);
	EXPECT_EQ(Texture2DDuplicate->GetSource().Payload.GetPayloadId(), Texture2DSourceId);
	ASSERT_NE(Texture2DDuplicate->GetAssetImportData(), nullptr);
	EXPECT_EQ(Texture2DDuplicate->GetAssetImportData()->GetOuter(), Texture2DDuplicate);
	ASSERT_TRUE(Durin::UnloadPackage(
		DuplicatePath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::SavePackage(Texture2D->GetPackage()));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "Texture2D.dasset",
		"/TextureLegacyFixtures/Texture2D", "Durin::DTexture", true));

	Durin::FPackagePath TextureCubePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureLegacyFixtures/TextureCube", TextureCubePath));
	Durin::DTextureCube* TextureCube = nullptr;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(TextureCubePath), TextureCube));
	ASSERT_NE(TextureCube, nullptr);
	EXPECT_TRUE(TextureCube->GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_FALSE(TextureCube->GetPackage()->IsDirty());
	EXPECT_EQ(TextureCube->GetSource().NumSlices, 6u);
	EXPECT_EQ(TextureCube->GetSourceLayout(), Durin::ETextureCubeSourceLayout::SixFaces);
	EXPECT_EQ(TextureCube->GetPanoramaFaceDimension(), 1u);
	EXPECT_FLOAT_EQ(TextureCube->GetPanoramaExposureEV(), 0.5f);
	EXPECT_FALSE(TextureCube->IsSRGB());
	ExpectImportData(*TextureCube, "TextureCube.source", 201);
	ExpectCanonicalOverride(*TextureCube, "Source");
	ExpectCanonicalOverride(*TextureCube, "AssetImportData");
	const Durin::FXxHash128 TextureCubeSourceId =
		TextureCube->GetSource().Payload.GetPayloadId();
	ASSERT_TRUE(Durin::SavePackage(TextureCube->GetPackage()));
	EXPECT_EQ(TextureCube->GetSource().Payload.GetPayloadId(), TextureCubeSourceId);
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "TextureCube.dasset",
		"/TextureLegacyFixtures/TextureCube", "Durin::DTexture", true));

	Durin::FPackagePath VolumePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureLegacyFixtures/VolumeTexture", VolumePath));
	Durin::DVolumeTexture* Volume = nullptr;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(VolumePath), Volume));
	ASSERT_NE(Volume, nullptr);
	EXPECT_TRUE(Volume->GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_FALSE(Volume->GetPackage()->IsDirty());
	EXPECT_EQ(Volume->GetSource().Width, 65u);
	EXPECT_EQ(Volume->GetSource().Height, 65u);
	EXPECT_EQ(Volume->GetSource().Depth, 65u);
	EXPECT_EQ(Volume->GetSource().Payload.GetPayloadSize(), 274625u);
	EXPECT_EQ(Volume->GetBuildSettings().OutputFormat, Durin::EVolumeTextureFormat::R8_UNORM);
	ExpectImportData(*Volume, "VolumeTexture.source", 301);
	ExpectCanonicalOverride(*Volume, "Source");
	ExpectCanonicalOverride(*Volume, "AssetImportData");
	const Durin::FXxHash128 VolumeSourceId = Volume->GetSource().Payload.GetPayloadId();
	ASSERT_TRUE(Durin::SavePackage(Volume->GetPackage()));
	EXPECT_EQ(Volume->GetSource().Payload.GetPayloadId(), VolumeSourceId);
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "VolumeTexture.dasset",
		"/TextureLegacyFixtures/VolumeTexture", "Durin::DTexture", true));
	Durin::FProperty* SourceProperty =
		Durin::DTexture2D::StaticClass()->FindPropertyByName("Source");
	Durin::FProperty* ImportProperty =
		Durin::DTextureCube::StaticClass()->FindPropertyByName("AssetImportData");
	ASSERT_NE(SourceProperty, nullptr);
	ASSERT_NE(ImportProperty, nullptr);
	EXPECT_EQ(SourceProperty->Owner.ToDObject(), Durin::DTexture::StaticClass());
	EXPECT_EQ(ImportProperty->Owner.ToDObject(), Durin::DTexture::StaticClass());
	ASSERT_TRUE(Durin::UnloadPackage(Texture2DPath));
	ASSERT_TRUE(Durin::UnloadPackage(TextureCubePath));
	ASSERT_TRUE(Durin::UnloadPackage(VolumePath));

	Texture2D = nullptr;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(Texture2DPath), Texture2D));
	ASSERT_NE(Texture2D, nullptr);
	EXPECT_EQ(Texture2D->GetSource().Payload.GetPayloadId(), Texture2DSourceId);
	ExpectImportData(*Texture2D, "Texture2D.source", 101);
	EXPECT_FALSE(Texture2D->GetPackage()->IsCanonicalResaveRecommended());
	TextureCube = nullptr;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(TextureCubePath), TextureCube));
	ASSERT_NE(TextureCube, nullptr);
	EXPECT_EQ(TextureCube->GetSource().Payload.GetPayloadId(), TextureCubeSourceId);
	ExpectImportData(*TextureCube, "TextureCube.source", 201);
	EXPECT_FALSE(TextureCube->GetPackage()->IsCanonicalResaveRecommended());
	Volume = nullptr;
	ASSERT_TRUE(Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(VolumePath), Volume));
	ASSERT_NE(Volume, nullptr);
	EXPECT_EQ(Volume->GetSource().Payload.GetPayloadId(), VolumeSourceId);
	ExpectImportData(*Volume, "VolumeTexture.source", 301);
	EXPECT_FALSE(Volume->GetPackage()->IsCanonicalResaveRecommended());
	ASSERT_TRUE(Durin::UnloadPackage(Texture2DPath));
	ASSERT_TRUE(Durin::UnloadPackage(TextureCubePath));
	ASSERT_TRUE(Durin::UnloadPackage(VolumePath));
}

TEST(FTextureBaseStateCompatibilityTests, PreMoveCookedFixturesKeepNativeFieldIdentities)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = CopyFixtureTree("CookedPackages");
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "Game/Texture2D.dasset",
		"/Game/Texture2D", "Durin::DTexture2D", false));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "Game/TextureCube.dasset",
		"/Game/TextureCube", "Durin::DTextureCube", false));
	ASSERT_NO_FATAL_FAILURE(InspectFixture(Root / "Game/VolumeTexture.dasset",
		"/Game/VolumeTexture", "Durin::DVolumeTexture", false));
	Durin::FAssetPackageInspection Inspection;
	Durin::FPackagePath PackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/Texture2D", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/Texture2D.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DTexture2D");
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/TextureCube", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/TextureCube.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DTextureCube");
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/VolumeTexture", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/VolumeTexture.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DVolumeTexture");

	Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
	ASSERT_TRUE(AssetRuntime.RestartCooked(Root));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));

	Durin::FPackagePath Texture2DPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/Texture2D", Texture2DPath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(Texture2DPath));
	Durin::DTexture2D* Texture2D = nullptr;
	const Durin::FAssetResult Texture2DLoad = Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(Texture2DPath), Texture2D);
	ASSERT_TRUE(Texture2DLoad) << Texture2DLoad.Message;
	ASSERT_NE(Texture2D, nullptr);
	EXPECT_FALSE(Texture2D->GetSource().IsValid());
	EXPECT_EQ(Texture2D->GetAssetImportData(), nullptr);
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*Texture2D));
	ASSERT_TRUE(Texture2D->GetPlatformData()->IsValid());
	EXPECT_EQ(Texture2D->GetPlatformData()->Mips.front().Width, 2u);
	EXPECT_EQ(Texture2D->GetPlatformData()->Mips.front().Height, 1u);

	Durin::FPackagePath TextureCubePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/TextureCube", TextureCubePath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(TextureCubePath));
	Durin::DTextureCube* TextureCube = nullptr;
	const Durin::FAssetResult TextureCubeLoad = Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(TextureCubePath), TextureCube);
	ASSERT_TRUE(TextureCubeLoad) << TextureCubeLoad.Message;
	ASSERT_NE(TextureCube, nullptr);
	EXPECT_FALSE(TextureCube->GetSource().IsValid());
	EXPECT_EQ(TextureCube->GetAssetImportData(), nullptr);
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*TextureCube));
	ASSERT_TRUE(TextureCube->GetPlatformData()->IsValid());
	EXPECT_EQ(TextureCube->GetBuiltFaceDimension(), 1u);

	Durin::FPackagePath VolumePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/VolumeTexture", VolumePath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(VolumePath));
	Durin::DVolumeTexture* Volume = nullptr;
	const Durin::FAssetResult VolumeLoad = Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(VolumePath), Volume);
	ASSERT_TRUE(VolumeLoad) << VolumeLoad.Message;
	ASSERT_NE(Volume, nullptr);
	EXPECT_FALSE(Volume->GetSource().IsValid());
	EXPECT_EQ(Volume->GetAssetImportData(), nullptr);
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*Volume));
	ASSERT_TRUE(Volume->GetPlatformData()->IsValid());
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Width, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Height, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Depth, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->PixelFormat, Durin::EPixelFormat::R8_UNORM);
	ASSERT_TRUE(Durin::UnloadPackage(Texture2DPath));
	ASSERT_TRUE(Durin::UnloadPackage(TextureCubePath));
	ASSERT_TRUE(Durin::UnloadPackage(VolumePath));
}
