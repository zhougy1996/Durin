#include "TextureTestSupport.h"

#include "Misc/FileHelper.h"
#include "StaticModelImportBuild.h"

namespace
{
	auto MakeAssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}

	auto MakeEmbeddedRequest(
		std::string_view AssetPath,
		std::string_view SourcePath,
		bool bRoot = false) -> Durin::FPortableTextureBuildRequest
	{
		return {
			.AssetPath = MakeAssetPath(AssetPath),
			.EncodedBytes = std::vector<Durin::uint8>(
				std::begin(TransparentPngBytes), std::end(TransparentPngBytes)),
			.SourceDestination = {.Path = std::string(SourcePath)},
			.Settings = {
				.Usage = Durin::ETextureUsage::Color,
				.bSRGB = true},
			.bRootPackage = bRoot};
	}

	auto PackageFile(std::string_view Name) -> std::filesystem::path
	{
		return std::filesystem::path(DURIN_TEST_WORK_DIR)
			/ "TextureImports" / "Content" / "StaticModelImport" / (std::string(Name) + ".dasset");
	}

	auto SourceFile(std::string_view Name) -> std::filesystem::path
	{
		return std::filesystem::path(DURIN_TEST_WORK_DIR)
			/ "TextureImports" / "SourceAssets" / "Models" / "Embedded" / std::string(Name);
	}
}

TEST(FStaticModelImportBuildTests, PublishesSeveralTexturesAndPortableSourcesAtomically)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportBuildSuccessDDC");

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture(MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/Color",
		"/TextureImportTests/Models/Embedded/Shared.png"));
	auto Linear = MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/Linear",
		"/TextureImportTests/Models/Embedded/Shared.png",
		true);
	Linear.Settings.Usage = Durin::ETextureUsage::DataMask;
	Linear.Settings.bSRGB = false;
	Transaction.AddTexture(std::move(Linear));

	const Durin::FImportTransactionResult Result = Transaction.Execute();
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Textures.size(), 2u);
	EXPECT_EQ(Result.Textures[0]->GetSourceFile(), "/TextureImportTests/Models/Embedded/Shared.png");
	EXPECT_EQ(Result.Textures[1]->GetSourceFile(), "/TextureImportTests/Models/Embedded/Shared.png");
	EXPECT_NE(Result.Textures[0]->GetDerivedDataKey(), Result.Textures[1]->GetDerivedDataKey());
	EXPECT_TRUE(std::filesystem::is_regular_file(SourceFile("Shared.png")));
	EXPECT_TRUE(std::filesystem::is_regular_file(PackageFile("Color")));
	EXPECT_TRUE(std::filesystem::is_regular_file(PackageFile("Linear")));
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(
		MakeAssetPath("/TextureImportTests/StaticModelImport/Color")), nullptr);
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(
		MakeAssetPath("/TextureImportTests/StaticModelImport/Linear")), nullptr);
	const Durin::FAssetPath ColorPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/Color");
	const Durin::FAssetPath LinearPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/Linear");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ColorPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LinearPath));
	Durin::DTexture2D* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ColorPath, Reloaded));
	EXPECT_EQ(Reloaded->GetSourceFile(), "/TextureImportTests/Models/Embedded/Shared.png");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ColorPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(ColorPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(LinearPath));
}

TEST(FStaticModelImportBuildTests, DerivesStableEmbeddedSourceLocations)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FSourcePath First;
	Durin::FSourcePath Repeat;
	Durin::FSourcePath Different;
	std::string Error;
	ASSERT_TRUE(Durin::BuildEmbeddedImageSourcePath(
		{.Path = "/TextureImportTests/Models/Robot.glb"},
		"glb:bufferView:2",
		"Base Color!",
		".PNG",
		First,
		Error)) << Error;
	ASSERT_TRUE(Durin::BuildEmbeddedImageSourcePath(
		{.Path = "/TextureImportTests/Models/Robot.glb"},
		"glb:bufferView:2",
		"Base Color!",
		".png",
		Repeat,
		Error)) << Error;
	ASSERT_TRUE(Durin::BuildEmbeddedImageSourcePath(
		{.Path = "/TextureImportTests/Models/Robot.glb"},
		"glb:bufferView:3",
		"Base Color!",
		".png",
		Different,
		Error)) << Error;
	EXPECT_EQ(First, Repeat);
	EXPECT_NE(First, Different);
	EXPECT_TRUE(First.Path.starts_with(
		"/TextureImportTests/Models/Robot_Embedded/Base_Color_"));
	EXPECT_TRUE(First.Path.ends_with(".png"));
}

TEST(FStaticModelImportBuildTests, ReferencesMountedExternalImageWithoutCopying)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportReferenceDDC");
	const std::filesystem::path Mounted =
		SourceFile("Mounted.png");
	std::filesystem::create_directories(Mounted.parent_path());
	WriteTextureFixture(Mounted);

	Durin::FMultiAssetImportTransaction Transaction;
	Durin::FPortableTextureBuildRequest Request{
		.AssetPath = MakeAssetPath("/TextureImportTests/StaticModelImport/Mounted"),
		.ExternalSource = Mounted,
		.Settings = {.Usage = Durin::ETextureUsage::Color, .bSRGB = true},
		.bRootPackage = true};
	Transaction.AddTexture(std::move(Request));
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Textures.size(), 1u);
	EXPECT_EQ(Result.Textures.front()->GetSourceFile(),
		"/TextureImportTests/Models/Embedded/Mounted.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(Mounted));
	const Durin::FAssetPath Path =
		MakeAssetPath("/TextureImportTests/StaticModelImport/Mounted");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
}

TEST(FStaticModelImportBuildTests, IngestsUnmountedExternalImageOnlyToExplicitWritableMount)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportIngestDDC");
	const std::filesystem::path External =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "ExternalModelImage.png";
	WriteTextureFixture(External);

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture({
		.AssetPath = MakeAssetPath("/TextureImportTests/StaticModelImport/Ingested"),
		.ExternalSource = External,
		.SourceDestination = {.Path = "/TextureImportTests/Models/Embedded/Ingested.png"},
		.Settings = {.Usage = Durin::ETextureUsage::Color, .bSRGB = true},
		.bRootPackage = true});
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Textures.size(), 1u);
	EXPECT_EQ(Result.Textures.front()->GetSourceFile(),
		"/TextureImportTests/Models/Embedded/Ingested.png");
	EXPECT_TRUE(std::filesystem::is_regular_file(SourceFile("Ingested.png")));
	const Durin::FAssetPath Path =
		MakeAssetPath("/TextureImportTests/StaticModelImport/Ingested");
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
}

TEST(FStaticModelImportBuildTests, EveryInjectedFailureRollsBackTheCompleteAttempt)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::array FailurePoints = {
		Durin::EImportTransactionFailurePoint::DirectoryCreation,
		Durin::EImportTransactionFailurePoint::SourceWrite,
		Durin::EImportTransactionFailurePoint::Decode,
		Durin::EImportTransactionFailurePoint::TextureBuild,
		Durin::EImportTransactionFailurePoint::DerivedDataPublication,
		Durin::EImportTransactionFailurePoint::PackageSave,
		Durin::EImportTransactionFailurePoint::RegistryPublication,
		Durin::EImportTransactionFailurePoint::RootPackageSave};

	for (size_t Index = 0; Index < FailurePoints.size(); ++Index)
	{
		SCOPED_TRACE(Index);
		FScopedDerivedDataCacheRoot CacheRoot(
			std::filesystem::path(DURIN_TEST_WORK_DIR)
			/ std::format("StaticModelImportFailureDDC{}", Index));
		const std::string AssetName = std::format("Failure{}", Index);
		const std::string SourceName = std::format("Failure{}.png", Index);
		const std::string AssetPath =
			std::format("/TextureImportTests/StaticModelImport/{}", AssetName);
		const std::string SourcePath =
			std::format("/TextureImportTests/Models/Embedded/{}", SourceName);
		int LoadedState = 7;

		Durin::FMultiAssetImportTransaction Transaction;
		Transaction.AddTexture(MakeEmbeddedRequest(AssetPath, SourcePath, true));
		Transaction.AddLoadedObjectMutation(
			[&](std::string&) {
				LoadedState = 99;
				return true;
			},
			[&] { LoadedState = 7; });
		Transaction.SetFailurePoint(FailurePoints[Index]);
		const Durin::FImportTransactionResult Result = Transaction.Execute();
		EXPECT_FALSE(Result);
		EXPECT_EQ(LoadedState, 7);
		const Durin::FAssetPath Parsed = MakeAssetPath(AssetPath);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(Parsed), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Parsed), nullptr);
		EXPECT_FALSE(std::filesystem::exists(PackageFile(AssetName)));
		EXPECT_FALSE(std::filesystem::exists(SourceFile(SourceName)));
		const std::filesystem::path DdcRoot =
			std::filesystem::path(Durin::FPaths::DerivedDataCacheDir()) / "Textures" / "Objects";
		size_t DerivedObjectCount = 0;
		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(DdcRoot, Ec), End;
			!Ec && It != End; It.increment(Ec))
			if (It->is_regular_file()) ++DerivedObjectCount;
		EXPECT_EQ(DerivedObjectCount, 0u);
	}
}

TEST(FStaticModelImportBuildTests, RollbackRestoresPreexistingLoadedObjectAndPackageBytes)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportLoadedRollbackDDC");
	const std::filesystem::path SeedSource =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportSeed.png";
	WriteTextureFixture(SeedSource);
	const Durin::FTexture2DImportResult Seed = Durin::DTexture2D::ImportAsset(
		SeedSource.generic_string(), "/TextureImportTests/StaticModelImport/Existing");
	ASSERT_TRUE(Seed) << Seed.Message;
	ASSERT_NE(Seed.Asset, nullptr);
	Durin::FProperty* LegacySourceProperty =
		Seed.Asset->GetClass()->FindPropertyByName("SourceFile");
	ASSERT_NE(LegacySourceProperty, nullptr);
	auto* LegacySource = static_cast<std::string*>(
		LegacySourceProperty->GetValuePtr(Seed.Asset));
	const std::string OriginalValue = *LegacySource;
	const bool bOriginalDirty = Seed.Asset->GetPackage()->IsDirty();
	const Durin::FAssetPath ExistingPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/Existing");
	const Durin::Asset::FAssetData OriginalRegistry =
		*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath);
	std::vector<Durin::uint8> OriginalPackageBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		OriginalPackageBytes, OriginalRegistry.PhysicalPath));

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddPackage(Seed.Asset->GetPackage());
	Transaction.AddTexture(MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/LoadedRollbackNew",
		"/TextureImportTests/Models/Embedded/LoadedRollbackNew.png",
		true));
	Transaction.AddLoadedObjectMutation(
		[&](std::string&) {
			*LegacySource = "mutated-during-attempt";
			Seed.Asset->MarkPackageDirty();
			return true;
		},
		[&] {
			*LegacySource = OriginalValue;
			if (bOriginalDirty) Seed.Asset->MarkPackageDirty();
			else Seed.Asset->GetPackage()->ClearDirty();
		});
	Transaction.SetFailurePoint(
		Durin::EImportTransactionFailurePoint::RegistryPublication);
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	EXPECT_FALSE(Result);
	EXPECT_EQ(*LegacySource, OriginalValue);
	EXPECT_EQ(Seed.Asset->GetPackage()->IsDirty(), bOriginalDirty);
	EXPECT_EQ(*Durin::Asset::GetAssetRegistry().FindAsset(ExistingPath), OriginalRegistry);
	std::vector<Durin::uint8> RestoredPackageBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		RestoredPackageBytes, OriginalRegistry.PhysicalPath));
	EXPECT_EQ(RestoredPackageBytes, OriginalPackageBytes);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(MakeAssetPath(
		"/TextureImportTests/StaticModelImport/LoadedRollbackNew")), nullptr);
	EXPECT_FALSE(std::filesystem::exists(SourceFile("LoadedRollbackNew.png")));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ExistingPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(ExistingPath));
}

TEST(FStaticModelImportBuildTests, SourceCollisionPreflightPreservesExistingBytes)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::filesystem::path Existing = SourceFile("Collision.png");
	std::filesystem::create_directories(Existing.parent_path());
	const std::array ExistingBytes = {Durin::uint8(1), Durin::uint8(2), Durin::uint8(3)};
	{
		std::ofstream Stream(Existing, std::ios::binary | std::ios::trunc);
		Stream.write(
			reinterpret_cast<const char*>(ExistingBytes.data()),
			static_cast<std::streamsize>(ExistingBytes.size()));
	}

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture(MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/Collision",
		"/TextureImportTests/Models/Embedded/Collision.png",
		true));
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	EXPECT_FALSE(Result);
	std::vector<Durin::uint8> Actual;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Actual, Existing.generic_string()));
	EXPECT_EQ(Actual, std::vector<Durin::uint8>(ExistingBytes.begin(), ExistingBytes.end()));
	EXPECT_FALSE(std::filesystem::exists(PackageFile("Collision")));
}

TEST(FStaticModelImportBuildTests, CompletesAllCollisionPreflightBeforeDdcOrSourceWrites)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticModelImportPreflightDDC");
	const std::filesystem::path BlockedPackage = PackageFile("PreflightBlocked");
	std::filesystem::create_directories(BlockedPackage.parent_path());
	{
		std::ofstream Stream(BlockedPackage, std::ios::binary | std::ios::trunc);
		Stream << "unrelated package bytes";
	}

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture(MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/PreflightFirst",
		"/TextureImportTests/Models/Embedded/PreflightFirst.png"));
	Transaction.AddTexture(MakeEmbeddedRequest(
		"/TextureImportTests/StaticModelImport/PreflightBlocked",
		"/TextureImportTests/Models/Embedded/PreflightBlocked.png",
		true));
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	EXPECT_FALSE(Result);
	EXPECT_FALSE(std::filesystem::exists(SourceFile("PreflightFirst.png")));
	EXPECT_FALSE(std::filesystem::exists(SourceFile("PreflightBlocked.png")));
	EXPECT_FALSE(std::filesystem::exists(PackageFile("PreflightFirst")));
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(MakeAssetPath(
		"/TextureImportTests/StaticModelImport/PreflightFirst")), nullptr);

	const std::filesystem::path DdcRoot =
		std::filesystem::path(Durin::FPaths::DerivedDataCacheDir()) / "Textures" / "Objects";
	size_t DerivedObjectCount = 0;
	std::error_code Ec;
	for (std::filesystem::recursive_directory_iterator It(DdcRoot, Ec), End;
		!Ec && It != End; It.increment(Ec))
		if (It->is_regular_file()) ++DerivedObjectCount;
	EXPECT_EQ(DerivedObjectCount, 0u);
}
