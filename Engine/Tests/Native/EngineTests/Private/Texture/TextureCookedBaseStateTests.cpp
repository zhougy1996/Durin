#include "TextureTestSupport.h"

#include "Asset/PackageInspection.h"
#include "Asset/Load.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace
{
	constexpr std::string_view FixtureDirectory = "TextureCookedBaseState";

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

	auto ExpectNoAuthoredState(const std::filesystem::path& Path,
		std::string_view PackageIdentity) -> void
	{
		Durin::FPackagePath PackagePath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(PackageIdentity, PackagePath)
			|| Durin::FPackagePath::TryCreateProjectContent(PackageIdentity, PackagePath));
		Durin::FAssetPackageInspection Inspection;
		ASSERT_TRUE(Durin::InspectAssetPackage(
			Path.generic_string(), PackagePath, Inspection));
		const Durin::FAssetPackageField* Source = Inspection.FindField("Source");
		EXPECT_EQ(Source, nullptr);
		const Durin::FAssetPackageField* ImportData = Inspection.FindField("AssetImportData");
		EXPECT_EQ(ImportData, nullptr);
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

TEST(FTextureCookedBaseStateTests, CookedFixturesKeepNativePlatformDataIdentities)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = CopyFixtureTree("CookedPackages");
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/Texture2D.dasset", "/Game/Texture2D"));
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/TextureCube.dasset", "/Game/TextureCube"));
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/VolumeTexture.dasset", "/Game/VolumeTexture"));
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
