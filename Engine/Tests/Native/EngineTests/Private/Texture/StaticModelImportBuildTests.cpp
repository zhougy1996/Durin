#include "TextureTestSupport.h"

#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "StaticModelImportBuild.h"
#include "StaticModelImportBuildInternal.h"

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
		return Durin::Testing::GetTestWorkDirectory()
			/ "TextureImports" / "Content" / "StaticModelImport" / (std::string(Name) + ".dasset");
	}

	auto SourceFile(std::string_view Name) -> std::filesystem::path
	{
		return Durin::Testing::GetTestWorkDirectory()
			/ "TextureImports" / "SourceAssets" / "Models" / "Embedded" / std::string(Name);
	}

	auto CountDerivedDataObjects() -> size_t
	{
		const std::filesystem::path Root =
			std::filesystem::path(Durin::FPaths::DerivedDataCacheDir()) / "Textures" / "Objects";
		size_t Count = 0;
		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(Root, Ec), End;
			!Ec && It != End; It.increment(Ec))
			if (It->is_regular_file()) ++Count;
		return Count;
	}
}

TEST(FStaticModelImportBuildTests, PublishesSeveralTexturesAndPortableSourcesAtomically)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportBuildSuccessDDC");

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

TEST(FStaticModelImportBuildTests, PublishesStandalonePortableSourceWithRootPackage)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/PortableSourceRoot");
	Durin::DMaterial* RootAsset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(RootPath, RootAsset));
	const std::filesystem::path External =
		Durin::Testing::GetTestWorkDirectory() / "PortableSourceRoot.glb";
	WriteTextureFixture(External);

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddPackage(RootAsset->GetPackage(), true);
	Transaction.AddSource({
		.AuthoringAssetPath = RootPath,
		.ExternalSource = External,
		.SourceDestination = {
			.Path = "/TextureImportTests/Models/PortableSourceRoot.glb"}});
	const Durin::FImportTransactionResult Result = Transaction.Execute();
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		SourceFile("../PortableSourceRoot.glb").lexically_normal()));
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(RootPath), nullptr);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	std::error_code ErrorCode;
	std::filesystem::remove(
		SourceFile("../PortableSourceRoot.glb").lexically_normal(), ErrorCode);
	EXPECT_FALSE(ErrorCode);
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

TEST(FStaticModelImportBuildTests, PlansDeterministicOpaqueModelOutputsWithoutMutation)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "StaticModelMaterials/MaterialContract.gltf";
	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/MaterialContract");
	const Durin::FStaticModelImportPlanRequest Request{
		.SourceFile = Source,
		.RootAssetPath = RootPath,
		.RootSourceDestination = {
			.Path = "/TextureImportTests/Models/MaterialContract.gltf"}};

	const Durin::FStaticModelImportPlanResult First =
		Durin::PlanStaticModelImport(Request);
	ASSERT_TRUE(First) << First.Message;
	const Durin::FStaticModelImportPlanResult Repeat =
		Durin::PlanStaticModelImport(Request);
	ASSERT_TRUE(Repeat) << Repeat.Message;

	EXPECT_EQ(First.Plan.StandardMaterialPath, Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_EQ(First.Plan.Assets.size(), 5u);
	EXPECT_EQ(First.Plan.Assets[0].Kind, Durin::EStaticModelPlannedAssetKind::StaticMesh);
	EXPECT_EQ(First.Plan.Assets[0].AssetPath, RootPath);
	EXPECT_EQ(
		First.Plan.Assets[1].AssetPath.ToString(),
		"/TextureImportTests/StaticModelImport/MaterialContract_Textures/RedPixel_BaseColor");
	EXPECT_EQ(First.Plan.Assets[1].Kind, Durin::EStaticModelPlannedAssetKind::Texture2D);
	EXPECT_EQ(
		First.Plan.Assets[2].AssetPath.ToString(),
		"/TextureImportTests/StaticModelImport/MaterialContract_Materials/Shared");
	EXPECT_EQ(
		First.Plan.Assets[3].AssetPath.ToString(),
		"/TextureImportTests/StaticModelImport/MaterialContract_Materials/Shared_2");
	EXPECT_EQ(
		First.Plan.Assets[4].AssetPath.ToString(),
		"/TextureImportTests/StaticModelImport/MaterialContract_Materials/Blend");
	for (size_t AssetIndex = 2; AssetIndex < First.Plan.Assets.size(); ++AssetIndex)
	{
		EXPECT_EQ(
			First.Plan.Assets[AssetIndex].Kind,
			Durin::EStaticModelPlannedAssetKind::MaterialInstance);
		ASSERT_TRUE(First.Plan.Assets[AssetIndex].TextureAssetIndex);
		EXPECT_EQ(*First.Plan.Assets[AssetIndex].TextureAssetIndex, 1u);
	}
	ASSERT_EQ(First.Plan.Sources.size(), 3u);
	EXPECT_EQ(First.Plan.Sources[0].Action, Durin::EStaticModelPlannedSourceAction::Ingest);
	EXPECT_EQ(
		First.Plan.Sources[0].SourcePath.Path,
		"/TextureImportTests/Models/MaterialContract.gltf");
	EXPECT_EQ(
		First.Plan.Sources[1].SourcePath.Path,
		"/TextureImportTests/Models/Triangle.bin");
	EXPECT_EQ(
		First.Plan.Sources[2].SourcePath.Path,
		"/TextureImportTests/Models/Red.png");
	EXPECT_TRUE(std::ranges::all_of(
		First.Plan.Sources,
		[](const Durin::FStaticModelPlannedSource& Planned) {
			return Planned.Action == Durin::EStaticModelPlannedSourceAction::Ingest;
		}));
	EXPECT_FALSE(First.Plan.Warnings.empty());

	ASSERT_EQ(First.Plan.Assets.size(), Repeat.Plan.Assets.size());
	for (size_t Index = 0; Index < First.Plan.Assets.size(); ++Index)
	{
		EXPECT_EQ(First.Plan.Assets[Index].Kind, Repeat.Plan.Assets[Index].Kind);
		EXPECT_EQ(First.Plan.Assets[Index].AssetPath, Repeat.Plan.Assets[Index].AssetPath);
		EXPECT_EQ(First.Plan.Assets[Index].SourceIndex, Repeat.Plan.Assets[Index].SourceIndex);
		EXPECT_EQ(
			First.Plan.Assets[Index].TextureAssetIndex,
			Repeat.Plan.Assets[Index].TextureAssetIndex);
	}
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(RootPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(RootPath), nullptr);

	Durin::DMaterial* Collision = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(First.Plan.Assets[1].AssetPath, Collision));
	const Durin::FStaticModelImportPlanResult Blocked =
		Durin::PlanStaticModelImport(Request);
	EXPECT_FALSE(Blocked);
	EXPECT_NE(Blocked.Message.find("collides with an existing asset"), std::string::npos);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(RootPath), nullptr);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(Collision->GetPackage()));
}

TEST(FStaticModelImportBuildTests, CreatesAndReloadsCanonicalStandardImportedSurfaceMaterial)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StandardImportedSurface";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Content");
	std::filesystem::create_directories(Root / "SourceAssets");
	const Durin::PathUtilities::FMountPoint EngineMount{
		.VirtualRoot = "/Engine/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.OwnerRoot = Root,
		.ContentRoot = Root / "Content",
		.SourceAssetsRoot = Root / "SourceAssets",
		.bSourceWritable = true};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(
		std::span<const Durin::PathUtilities::FMountPoint>(&EngineMount, 1));

	std::string Error;
	Durin::DMaterial* Created = Durin::EnsureStandardImportedSurfaceMaterial(Error);
	ASSERT_NE(Created, nullptr) << Error;
	EXPECT_TRUE(Durin::ValidateCanonicalMaterialParameterDefinitions(
		Created->GetParameterDefinitions(), Error)) << Error;
	EXPECT_EQ(Created, Durin::EnsureStandardImportedSurfaceMaterial(Error));

	const Durin::FAssetPath MaterialPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	Durin::DMaterial* Reloaded = Durin::EnsureStandardImportedSurfaceMaterial(Error);
	ASSERT_NE(Reloaded, nullptr) << Error;
	EXPECT_TRUE(Durin::ValidateCanonicalMaterialParameterDefinitions(
		Reloaded->GetParameterDefinitions(), Error)) << Error;
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(MaterialPath), nullptr);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MaterialPath));

	Durin::DTexture2D* WrongType = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(MaterialPath, WrongType));
	EXPECT_EQ(Durin::EnsureStandardImportedSurfaceMaterial(Error), nullptr);
	EXPECT_NE(Error.find("is occupied by"), std::string::npos);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(WrongType->GetPackage()));
}

TEST(FStaticModelImportBuildTests, ReferencesMountedExternalImageWithoutCopying)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportReferenceDDC");
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
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportIngestDDC");
	const std::filesystem::path External =
		Durin::Testing::GetTestWorkDirectory() / "ExternalModelImage.png";
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
		Durin::EImportTransactionFailurePoint::SourcePublication,
		Durin::EImportTransactionFailurePoint::Decode,
		Durin::EImportTransactionFailurePoint::TextureBuild,
		Durin::EImportTransactionFailurePoint::DerivedDataPublication,
		Durin::EImportTransactionFailurePoint::PackageStaging,
		Durin::EImportTransactionFailurePoint::DependencyPackagePublication,
		Durin::EImportTransactionFailurePoint::RegistryPublication,
		Durin::EImportTransactionFailurePoint::RootPackagePublication};

	for (size_t Index = 0; Index < FailurePoints.size(); ++Index)
	{
		SCOPED_TRACE(Index);
		FScopedDerivedDataCacheRoot CacheRoot(
			Durin::Testing::GetTestWorkDirectory()
			/ std::format("StaticModelImportFailureDDC{}", Index));
		const std::string AssetName = std::format("Failure{}", Index);
		const std::string SourceName = std::format("Failure{}.png", Index);
		const std::string AssetPath =
			std::format("/TextureImportTests/StaticModelImport/{}", AssetName);
		const std::string SourcePath =
			std::format("/TextureImportTests/Models/Embedded/{}", SourceName);
		int LoadedState = 7;

		Durin::FMultiAssetImportTransaction Transaction;
		const bool bNeedsDependency =
			FailurePoints[Index]
				== Durin::EImportTransactionFailurePoint::DependencyPackagePublication;
		if (bNeedsDependency)
		{
			Transaction.AddTexture(MakeEmbeddedRequest(
				AssetPath + "Dependency",
				SourcePath + ".dependency"));
		}
		Transaction.AddTexture(MakeEmbeddedRequest(AssetPath, SourcePath, true));
		Transaction.AddLoadedObjectMutation(
			[&](std::string&) {
				LoadedState = 99;
				return true;
			},
			[&] { LoadedState = 7; });
		Durin::FMultiAssetImportTransactionTestAccess::SetFailurePoint(
			Transaction, FailurePoints[Index]);
		const Durin::FImportTransactionResult Result = Transaction.Execute();
		EXPECT_FALSE(Result);
		EXPECT_EQ(LoadedState, 7);
		const Durin::FAssetPath Parsed = MakeAssetPath(AssetPath);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(Parsed), nullptr);
		EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Parsed), nullptr);
		EXPECT_FALSE(std::filesystem::exists(PackageFile(AssetName)));
		EXPECT_FALSE(std::filesystem::exists(SourceFile(SourceName)));
		if (bNeedsDependency)
		{
			const Durin::FAssetPath DependencyPath = MakeAssetPath(AssetPath + "Dependency");
			EXPECT_EQ(Durin::Asset::FindLoadedPackage(DependencyPath), nullptr);
			EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(DependencyPath), nullptr);
			EXPECT_FALSE(std::filesystem::exists(PackageFile(AssetName + "Dependency")));
			EXPECT_FALSE(std::filesystem::exists(SourceFile(SourceName + ".dependency")));
		}
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
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportLoadedRollbackDDC");
	const std::filesystem::path SeedSource =
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportSeed.png";
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
	Durin::FMultiAssetImportTransactionTestAccess::SetFailurePoint(
		Transaction, Durin::EImportTransactionFailurePoint::RegistryPublication);
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

TEST(FStaticModelImportBuildTests, LaterCandidateAndDependencyFailuresRollBackEarlierWork)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::array FailurePoints = {
		Durin::EImportTransactionFailurePoint::Decode,
		Durin::EImportTransactionFailurePoint::TextureBuild,
		Durin::EImportTransactionFailurePoint::DerivedDataPublication,
		Durin::EImportTransactionFailurePoint::DependencyPackagePublication};

	for (size_t FailureIndex = 0; FailureIndex < FailurePoints.size(); ++FailureIndex)
	{
		SCOPED_TRACE(FailureIndex);
		FScopedDerivedDataCacheRoot CacheRoot(
			Durin::Testing::GetTestWorkDirectory()
			/ std::format("StaticModelImportLaterFailureDDC{}", FailureIndex));
		const size_t TextureCount =
			FailurePoints[FailureIndex]
				== Durin::EImportTransactionFailurePoint::DependencyPackagePublication
			? 3u
			: 2u;

		Durin::FMultiAssetImportTransaction Transaction;
		for (size_t TextureIndex = 0; TextureIndex < TextureCount; ++TextureIndex)
		{
			Transaction.AddTexture(MakeEmbeddedRequest(
				std::format(
					"/TextureImportTests/StaticModelImport/LaterFailure{}_{}",
					FailureIndex,
					TextureIndex),
				std::format(
					"/TextureImportTests/Models/Embedded/LaterFailure{}_{}.png",
					FailureIndex,
					TextureIndex),
				TextureIndex + 1 == TextureCount));
		}
		Durin::FMultiAssetImportTransactionTestAccess::SetFailurePoint(
			Transaction, FailurePoints[FailureIndex], 1);

		const Durin::FImportTransactionResult Result = Transaction.Execute();
		EXPECT_FALSE(Result);
		for (size_t TextureIndex = 0; TextureIndex < TextureCount; ++TextureIndex)
		{
			const std::string Name =
				std::format("LaterFailure{}_{}", FailureIndex, TextureIndex);
			const Durin::FAssetPath Path = MakeAssetPath(
				std::format("/TextureImportTests/StaticModelImport/{}", Name));
			EXPECT_EQ(Durin::Asset::FindLoadedPackage(Path), nullptr);
			EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
			EXPECT_FALSE(std::filesystem::exists(PackageFile(Name)));
			EXPECT_FALSE(std::filesystem::exists(SourceFile(Name + ".png")));
		}
		EXPECT_EQ(CountDerivedDataObjects(), 0u);
	}
}

TEST(FStaticModelImportBuildTests, ExplicitAndDestructorRollbackRestorePreparedAttempts)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportLifecycleDDC");
	const Durin::FAssetPath ExplicitPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/ExplicitRollback");
	{
		Durin::FMultiAssetImportTransaction Transaction;
		Transaction.AddTexture(MakeEmbeddedRequest(
			ExplicitPath.ToString(),
			"/TextureImportTests/Models/Embedded/ExplicitRollback.png",
			true));
		std::string Error;
		ASSERT_TRUE(Transaction.Prepare(Error)) << Error;
		EXPECT_FALSE(Transaction.Prepare(Error));
		Transaction.Rollback();
	}
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(ExplicitPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(SourceFile("ExplicitRollback.png")));
	EXPECT_EQ(CountDerivedDataObjects(), 0u);

	const Durin::FAssetPath DestructorPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/DestructorRollback");
	{
		Durin::FMultiAssetImportTransaction Transaction;
		Transaction.AddTexture(MakeEmbeddedRequest(
			DestructorPath.ToString(),
			"/TextureImportTests/Models/Embedded/DestructorRollback.png",
			true));
		std::string Error;
		ASSERT_TRUE(Transaction.Prepare(Error)) << Error;
	}
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(DestructorPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(SourceFile("DestructorRollback.png")));
	EXPECT_EQ(CountDerivedDataObjects(), 0u);
}

TEST(FStaticModelImportBuildTests, PublishedAttemptSurvivesTransactionDestruction)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportPublishedOwnershipDDC");
	const Durin::FAssetPath Path =
		MakeAssetPath("/TextureImportTests/StaticModelImport/PublishedOwnership");
	{
		Durin::FMultiAssetImportTransaction Transaction;
		Transaction.AddTexture(MakeEmbeddedRequest(
			Path.ToString(),
			"/TextureImportTests/Models/Embedded/PublishedOwnership.png",
			true));
		const Durin::FImportTransactionResult Result = Transaction.Execute();
		ASSERT_TRUE(Result) << Result.Message;
	}
	EXPECT_NE(Durin::Asset::FindLoadedPackage(Path), nullptr);
	EXPECT_NE(Durin::Asset::GetAssetRegistry().FindAsset(Path), nullptr);
	EXPECT_TRUE(std::filesystem::is_regular_file(PackageFile("PublishedOwnership")));
	EXPECT_TRUE(std::filesystem::is_regular_file(SourceFile("PublishedOwnership.png")));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(Path));
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
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportPreflightDDC");
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

TEST(FStaticModelImportBuildTests, RejectsExternalInputChangedAfterPreparation)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportChangedInputDDC");
	const std::filesystem::path External =
		Durin::Testing::GetTestWorkDirectory() / "ChangedExternalModelImage.png";
	WriteTextureFixture(External);

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture({
		.AssetPath = MakeAssetPath("/TextureImportTests/StaticModelImport/ChangedInput"),
		.ExternalSource = External,
		.SourceDestination = {.Path = "/TextureImportTests/Models/Embedded/ChangedInput.png"},
		.Settings = {.Usage = Durin::ETextureUsage::Color, .bSRGB = true},
		.bRootPackage = true});
	std::string Error;
	ASSERT_TRUE(Transaction.Prepare(Error)) << Error;
	{
		std::ofstream Stream(External, std::ios::binary | std::ios::trunc);
		Stream << "changed after resolved plan construction";
	}

	EXPECT_FALSE(Transaction.Stage(Error));
	EXPECT_NE(Error.find("changed after transaction resolution"), std::string::npos);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(MakeAssetPath(
		"/TextureImportTests/StaticModelImport/ChangedInput")), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(MakeAssetPath(
		"/TextureImportTests/StaticModelImport/ChangedInput")), nullptr);
	EXPECT_FALSE(std::filesystem::exists(PackageFile("ChangedInput")));
	EXPECT_FALSE(std::filesystem::exists(SourceFile("ChangedInput.png")));

	const std::filesystem::path DdcRoot =
		std::filesystem::path(Durin::FPaths::DerivedDataCacheDir()) / "Textures" / "Objects";
	size_t DerivedObjectCount = 0;
	std::error_code Ec;
	for (std::filesystem::recursive_directory_iterator It(DdcRoot, Ec), End;
		!Ec && It != End; It.increment(Ec))
		if (It->is_regular_file()) ++DerivedObjectCount;
	EXPECT_EQ(DerivedObjectCount, 0u);
}
