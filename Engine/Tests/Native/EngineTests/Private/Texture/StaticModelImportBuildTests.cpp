#include "TextureTestSupport.h"

#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
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

TEST(FStaticModelImportBuildTests, ExecutesOpaqueGlbModelBundleAndReloadsReferences)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "OpaqueModelBundle";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
	{
		std::filesystem::create_directories(Directory);
	}
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TextureImportTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);

	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/Imports/EmbeddedGlbModel");
	const Durin::FStaticModelImportPlanResult Planned =
		Durin::PlanStaticModelImport({
			.SourceFile = std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/EmbeddedImage.glb",
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/TextureImportTests/Models/EmbeddedImage.glb"}});
	ASSERT_TRUE(Planned) << Planned.Message;
	ASSERT_EQ(Planned.Plan.Assets.size(), 3u);

	const Durin::FStaticModelImportExecutionResult Executed =
		Durin::ExecuteStaticModelImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_NE(Executed.StaticMesh, nullptr);
	ASSERT_EQ(Executed.Materials.size(), 1u);
	ASSERT_EQ(Executed.Textures.size(), 1u);
	EXPECT_EQ(Executed.Textures[0]->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Executed.Textures[0]->IsSRGB());
	const Durin::FStaticMeshMaterialSlotDefinition* Slot =
		Executed.StaticMesh->GetMaterialSlot(0);
	ASSERT_NE(Slot, nullptr);
	EXPECT_EQ(Slot->DefaultMaterial.Get(), Executed.Materials[0]);
	EXPECT_EQ(Executed.Materials[0]->GetParent()->GetPackage()->GetPackagePath(),
		Durin::StandardImportedSurfaceMaterialPath);
	Durin::FVector3 BaseColor;
	float Opacity = 0.0f;
	Durin::DTexture2D* BaseColorTexture = nullptr;
	ASSERT_TRUE(Executed.Materials[0]->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), BaseColor));
	ASSERT_TRUE(Executed.Materials[0]->GetScalarParameterValue(
		Durin::MaterialParameters::OpacityName(), Opacity));
	ASSERT_TRUE(Executed.Materials[0]->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), BaseColorTexture));
	EXPECT_EQ(BaseColor, Durin::FVector3(1.0, 1.0, 1.0));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	EXPECT_EQ(BaseColorTexture, Executed.Textures[0]);
	const Durin::FStaticModelImportManifest& Manifest =
		Executed.StaticMesh->GetImportManifest();
	EXPECT_TRUE(Manifest.IsValid());
	ASSERT_EQ(Manifest.Dependencies.size(), 1u);
	ASSERT_EQ(Manifest.Materials.size(), 1u);
	ASSERT_EQ(Manifest.Textures.size(), 1u);
	EXPECT_EQ(Manifest.Dependencies[0].SourcePath, Planned.Plan.RootSource);
	EXPECT_EQ(Manifest.Materials[0].GeneratedMaterial.Get(), Executed.Materials[0]);
	EXPECT_EQ(Manifest.Textures[0].GeneratedTexture.Get(), Executed.Textures[0]);
	const std::string DependencyFingerprint = Manifest.DependencyFingerprint;
	Durin::DStaticMeshComponent* Component =
		Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "ImportedComponent");
	Component->SetStaticMesh(Executed.StaticMesh);
	EXPECT_EQ(Component->GetMaterial(0), Executed.Materials[0]);
	Durin::MarkAsGarbage(Component);

	const Durin::FAssetPath TexturePath = Planned.Plan.Assets[1].AssetPath;
	const Durin::FAssetPath MaterialPath = Planned.Plan.Assets[2].AssetPath;
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));

	Durin::DStaticMesh* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(RootPath, Reloaded));
	EXPECT_FALSE(Reloaded->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	ASSERT_NE(Reloaded->GetMaterialSlot(0), nullptr);
	auto* ReloadedMaterial =
		Durin::Cast<Durin::DMaterialInstance>(Reloaded->GetMaterialSlot(0)->DefaultMaterial.Get());
	ASSERT_NE(ReloadedMaterial, nullptr);
	Durin::DTexture2D* ReloadedTexture = nullptr;
	ASSERT_TRUE(ReloadedMaterial->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_NE(ReloadedTexture, nullptr);
	EXPECT_TRUE(ReloadedTexture->IsSRGB());
	const Durin::FStaticModelImportManifest& ReloadedManifest =
		Reloaded->GetImportManifest();
	EXPECT_TRUE(ReloadedManifest.IsValid());
	EXPECT_EQ(ReloadedManifest.DependencyFingerprint, DependencyFingerprint);
	ASSERT_EQ(ReloadedManifest.Materials.size(), 1u);
	ASSERT_EQ(ReloadedManifest.Textures.size(), 1u);
	EXPECT_EQ(
		ReloadedManifest.Materials[0].GeneratedMaterial.Get(),
		ReloadedMaterial);
	EXPECT_EQ(
		ReloadedManifest.Textures[0].GeneratedTexture.Get(),
		ReloadedTexture);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	Durin::Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticModelImportBuildTests, ReimportsManagedGraphInPlaceAndReportsTextureOrphan)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticModelManagedReimport";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
	{
		std::filesystem::create_directories(Directory);
	}
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TextureImportTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);

	const std::filesystem::path InitialFixture =
		std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf";
	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/Reimport/ManagedGraph");
	const Durin::FStaticModelImportPlanResult InitialPlan =
		Durin::PlanStaticModelImport({
			.SourceFile = InitialFixture,
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/TextureImportTests/Models/ManagedGraph.gltf"}});
	ASSERT_TRUE(InitialPlan) << InitialPlan.Message;
	const Durin::FStaticModelImportExecutionResult Initial =
		Durin::ExecuteStaticModelImport(InitialPlan.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	ASSERT_EQ(Initial.Materials.size(), 1u);
	ASSERT_EQ(Initial.Textures.size(), 1u);
	Durin::DMaterialInstance* const OriginalMaterial = Initial.Materials[0];
	Durin::DTexture2D* const OriginalTexture = Initial.Textures[0];
	const Durin::FGuid OriginalSlotId =
		Initial.StaticMesh->GetMaterialSlot(0)->SlotId;
	const std::string OriginalTextureHash =
		OriginalTexture->GetSourceContentHash();

	const Durin::FStaticModelImportPlanResult UnchangedPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(UnchangedPlan) << UnchangedPlan.Message;
	const Durin::FStaticModelImportExecutionResult Unchanged =
		Durin::ExecuteStaticModelImport(UnchangedPlan.Plan);
	ASSERT_TRUE(Unchanged) << Unchanged.Message;
	EXPECT_EQ(Unchanged.StaticMesh, Initial.StaticMesh);
	ASSERT_EQ(Unchanged.Materials.size(), 1u);
	ASSERT_EQ(Unchanged.Textures.size(), 1u);
	EXPECT_EQ(Unchanged.Materials[0], OriginalMaterial);
	EXPECT_EQ(Unchanged.Textures[0], OriginalTexture);
	EXPECT_EQ(Unchanged.StaticMesh->GetMaterialSlot(0)->SlotId, OriginalSlotId);
	EXPECT_TRUE(Unchanged.OrphanedAssets.empty());

	const Durin::FAssetPath MovedMaterialPath =
		MakeAssetPath("/TextureImportTests/Reimport/UserMovedMaterial");
	const Durin::FAssetPath OriginalMaterialPath =
		InitialPlan.Plan.Assets[2].AssetPath;
	ASSERT_TRUE(Durin::Asset::MoveAsset(
		OriginalMaterialPath, MovedMaterialPath));
	EXPECT_EQ(
		OriginalMaterial->GetPackage()->GetPackagePath(),
		MovedMaterialPath.ToString());

	const std::filesystem::path ManagedSource =
		Root / "Project/SourceAssets/Models/ManagedGraph.gltf";
	std::string ChangedSource;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToString(
		ChangedSource, InitialFixture.generic_string()));
	const std::string OldImage =
		"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAANSURBVBhXY2hwOPAfAAVEAoBAoAl1AAAAAElFTkSuQmCC";
	const std::string NewImage =
		"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZlL8AAAAASUVORK5CYII=";
	ASSERT_NE(ChangedSource.find(OldImage), std::string::npos);
	ChangedSource.replace(
		ChangedSource.find(OldImage), OldImage.size(), NewImage);
	ASSERT_NE(ChangedSource.find("RenderedOpaque"), std::string::npos);
	ChangedSource.replace(
		ChangedSource.find("RenderedOpaque"),
		std::string("RenderedOpaque").size(),
		"RenamedOpaque");
	ASSERT_NE(
		ChangedSource.find("[0.5, 0.75, 0.25, 1.0]"),
		std::string::npos);
	ChangedSource.replace(
		ChangedSource.find("[0.5, 0.75, 0.25, 1.0]"),
		std::string("[0.5, 0.75, 0.25, 1.0]").size(),
		"[0.25, 0.5, 0.75, 1.0]");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{ChangedSource}), ManagedSource));

	const Durin::FStaticModelImportPlanResult ChangedPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(ChangedPlan) << ChangedPlan.Message;
	const Durin::FStaticModelImportExecutionResult Changed =
		Durin::ExecuteStaticModelImport(ChangedPlan.Plan);
	ASSERT_TRUE(Changed) << Changed.Message;
	ASSERT_EQ(Changed.Materials.size(), 1u);
	ASSERT_EQ(Changed.Textures.size(), 1u);
	EXPECT_EQ(Changed.Materials[0], OriginalMaterial);
	EXPECT_EQ(Changed.Textures[0], OriginalTexture);
	EXPECT_EQ(
		Changed.Materials[0]->GetPackage()->GetPackagePath(),
		MovedMaterialPath.ToString());
	EXPECT_EQ(Changed.StaticMesh->GetMaterialSlot(0)->SlotId, OriginalSlotId);
	EXPECT_NE(Changed.Textures[0]->GetSourceContentHash(), OriginalTextureHash);
	Durin::FVector3 ChangedFactor;
	ASSERT_TRUE(Changed.Materials[0]->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), ChangedFactor));
	EXPECT_EQ(ChangedFactor, Durin::FVector3(0.25, 0.5, 0.75));
	ASSERT_EQ(Changed.StaticMesh->GetImportManifest().Materials.size(), 1u);
	EXPECT_TRUE(
		Changed.StaticMesh->GetImportManifest().Materials[0].bImporterManaged);
	EXPECT_EQ(
		Changed.StaticMesh->GetImportManifest().Materials[0].GeneratedMaterialPath,
		MovedMaterialPath);

	ASSERT_NE(ChangedSource.find(NewImage), std::string::npos);
	ChangedSource.replace(
		ChangedSource.find(NewImage), NewImage.size(), OldImage);
	ASSERT_NE(
		ChangedSource.find("[0.25, 0.5, 0.75, 1.0]"),
		std::string::npos);
	ChangedSource.replace(
		ChangedSource.find("[0.25, 0.5, 0.75, 1.0]"),
		std::string("[0.25, 0.5, 0.75, 1.0]").size(),
		"[0.3, 0.4, 0.5, 1.0]");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{ChangedSource}), ManagedSource));
	const Durin::PathUtilities::FSourcePathResult ExtractedSource =
		Durin::PathUtilities::ResolveSourcePath(
			OriginalTexture->GetSourceFile(),
			Durin::PathUtilities::EPathExistence::RequireFile);
	ASSERT_TRUE(ExtractedSource) << ExtractedSource.Message;
	std::vector<Durin::uint8> ExtractedBytesBeforeFailure;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExtractedBytesBeforeFailure,
		ExtractedSource.PhysicalPath.generic_string()));
	Durin::FStaticModelImportPlanResult FailedPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(FailedPlan) << FailedPlan.Message;
	Durin::FStaticModelImportExecutionTestAccess::SetFailurePoint(
		FailedPlan.Plan,
		Durin::EImportTransactionFailurePoint::RootPackagePublication);
	const std::string BeforeFailedFingerprint =
		Changed.StaticMesh->GetImportManifest().DependencyFingerprint;
	const std::string BeforeFailedTextureHash =
		OriginalTexture->GetSourceContentHash();
	const Durin::FStaticModelImportExecutionResult Failed =
		Durin::ExecuteStaticModelImport(FailedPlan.Plan);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(
		Changed.StaticMesh->GetImportManifest().DependencyFingerprint,
		BeforeFailedFingerprint);
	EXPECT_EQ(OriginalTexture->GetSourceContentHash(), BeforeFailedTextureHash);
	EXPECT_EQ(
		Changed.StaticMesh->GetMaterialSlot(0)->DefaultMaterial.Get(),
		OriginalMaterial);
	std::vector<Durin::uint8> ExtractedBytesAfterFailure;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ExtractedBytesAfterFailure,
		ExtractedSource.PhysicalPath.generic_string()));
	EXPECT_EQ(ExtractedBytesAfterFailure, ExtractedBytesBeforeFailure);
	ASSERT_TRUE(OriginalMaterial->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), ChangedFactor));
	EXPECT_EQ(ChangedFactor, Durin::FVector3(0.25, 0.5, 0.75));

	std::vector<Durin::uint8> NoTextureSource;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		NoTextureSource,
		(std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/RenderedOpaqueNoTexture.gltf").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{NoTextureSource}), ManagedSource));
	const Durin::FStaticModelImportPlanResult RemovedTexturePlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(RemovedTexturePlan) << RemovedTexturePlan.Message;
	const Durin::FStaticModelImportExecutionResult RemovedTexture =
		Durin::ExecuteStaticModelImport(RemovedTexturePlan.Plan);
	ASSERT_TRUE(RemovedTexture) << RemovedTexture.Message;
	EXPECT_TRUE(RemovedTexture.Textures.empty());
	ASSERT_EQ(RemovedTexture.OrphanedAssets.size(), 1u);
	EXPECT_EQ(
		RemovedTexture.OrphanedAssets[0],
		InitialPlan.Plan.Assets[1].AssetPath);
	EXPECT_NE(
		Durin::Asset::GetAssetRegistry().FindAsset(
			InitialPlan.Plan.Assets[1].AssetPath),
		nullptr);

	const Durin::FAssetPath TexturePath = InitialPlan.Plan.Assets[1].AssetPath;
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MovedMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	Durin::DStaticMesh* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(RootPath, Reloaded));
	ASSERT_EQ(Reloaded->GetImportManifest().Materials.size(), 1u);
	EXPECT_TRUE(Reloaded->GetImportManifest().Materials[0].bImporterManaged);
	ASSERT_NE(
		Reloaded->GetImportManifest().Materials[0].GeneratedMaterial.Get(),
		nullptr);
	EXPECT_EQ(
		Reloaded->GetImportManifest().Materials[0]
			.GeneratedMaterial->GetPackage()->GetPackagePath(),
		MovedMaterialPath.ToString());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MovedMaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MovedMaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	Durin::Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticModelImportBuildTests, ReconcilesReorderedAddedAndRemovedMaterialsBySlotIdentity)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticModelMaterialReimport";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
		std::filesystem::create_directories(Directory);
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TextureImportTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const auto Fixture = [](std::string_view Name) {
		return std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials" / Name;
	};
	const auto ReplaceManagedSource = [&](std::string_view Name) {
		std::vector<Durin::uint8> Bytes;
		EXPECT_TRUE(Durin::FFileHelper::LoadFileToArray(
			Bytes, Fixture(Name).generic_string()));
		EXPECT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span{Bytes}),
			Root / "Project/SourceAssets/Models/MaterialReimport.gltf"));
	};

	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/Reimport/MaterialGraph");
	const Durin::FStaticModelImportPlanResult InitialPlan =
		Durin::PlanStaticModelImport({
			.SourceFile = Fixture("ReimportMaterialsInitial.gltf"),
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/TextureImportTests/Models/MaterialReimport.gltf"}});
	ASSERT_TRUE(InitialPlan) << InitialPlan.Message;
	const Durin::FStaticModelImportExecutionResult Initial =
		Durin::ExecuteStaticModelImport(InitialPlan.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	ASSERT_EQ(Initial.Materials.size(), 2u);
	const auto& InitialManifest = Initial.StaticMesh->GetImportManifest();
	ASSERT_EQ(InitialManifest.Materials.size(), 2u);
	Durin::DMaterialInstance* RedMaterial = nullptr;
	Durin::DMaterialInstance* BlueMaterial = nullptr;
	Durin::FGuid RedSlot;
	Durin::FGuid BlueSlot;
	for (const Durin::FStaticModelImportMaterialRecord& Record :
		InitialManifest.Materials)
	{
		if (Record.SourceName == "Red")
		{
			RedMaterial = Record.GeneratedMaterial.Get();
			RedSlot = Record.SlotId;
		}
		else if (Record.SourceName == "Blue")
		{
			BlueMaterial = Record.GeneratedMaterial.Get();
			BlueSlot = Record.SlotId;
		}
	}
	ASSERT_NE(RedMaterial, nullptr);
	ASSERT_NE(BlueMaterial, nullptr);
	Durin::DStaticMeshComponent* Component =
		Durin::NewObject<Durin::DStaticMeshComponent>(
			nullptr, "ReimportOverrideComponent");
	Component->SetStaticMesh(Initial.StaticMesh);
	ASSERT_TRUE(Component->SetMaterialBySlotId(RedSlot, BlueMaterial));

	ReplaceManagedSource("ReimportMaterialsReorderedAdded.gltf");
	const Durin::FStaticModelImportPlanResult AddedPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(AddedPlan) << AddedPlan.Message;
	const Durin::FStaticModelImportExecutionResult Added =
		Durin::ExecuteStaticModelImport(AddedPlan.Plan);
	ASSERT_TRUE(Added) << Added.Message;
	ASSERT_EQ(Added.Materials.size(), 3u);
	const auto& AddedManifest = Added.StaticMesh->GetImportManifest();
	ASSERT_EQ(AddedManifest.Materials.size(), 3u);
	Durin::DMaterialInstance* GreenMaterial = nullptr;
	for (const Durin::FStaticModelImportMaterialRecord& Record :
		AddedManifest.Materials)
	{
		if (Record.SourceName == "Red")
		{
			EXPECT_EQ(Record.GeneratedMaterial.Get(), RedMaterial);
			EXPECT_EQ(Record.SlotId, RedSlot);
			EXPECT_EQ(Record.SourceMaterialIndex, 1u);
		}
		else if (Record.SourceName == "Blue")
		{
			EXPECT_EQ(Record.GeneratedMaterial.Get(), BlueMaterial);
			EXPECT_EQ(Record.SlotId, BlueSlot);
			EXPECT_EQ(Record.SourceMaterialIndex, 0u);
		}
		else if (Record.SourceName == "Green")
		{
			GreenMaterial = Record.GeneratedMaterial.Get();
		}
	}
	ASSERT_NE(GreenMaterial, nullptr);
	EXPECT_TRUE(Component->HasMaterialOverride(RedSlot));
	EXPECT_EQ(Component->GetMaterialBySlotId(RedSlot), BlueMaterial);

	ReplaceManagedSource("ReimportMaterialsRemoved.gltf");
	const Durin::FStaticModelImportPlanResult RemovedPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	ASSERT_TRUE(RemovedPlan) << RemovedPlan.Message;
	const Durin::FStaticModelImportExecutionResult Removed =
		Durin::ExecuteStaticModelImport(RemovedPlan.Plan);
	ASSERT_TRUE(Removed) << Removed.Message;
	ASSERT_EQ(Removed.Materials.size(), 2u);
	ASSERT_EQ(Removed.OrphanedAssets.size(), 1u);
	EXPECT_EQ(
		Removed.OrphanedAssets[0],
		InitialPlan.Plan.Assets[2].AssetPath);
	EXPECT_NE(
		Durin::Asset::GetAssetRegistry().FindAsset(Removed.OrphanedAssets[0]),
		nullptr);
	EXPECT_TRUE(Component->HasMaterialOverride(RedSlot));
	EXPECT_EQ(Component->GetMaterialBySlotId(RedSlot), BlueMaterial);
	Durin::MarkAsGarbage(Component);

	const Durin::FAssetPath RedPath = InitialPlan.Plan.Assets[1].AssetPath;
	const Durin::FAssetPath BluePath = InitialPlan.Plan.Assets[2].AssetPath;
	const Durin::FAssetPath GreenPath = AddedPlan.Plan.Assets[3].AssetPath;
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RedPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(BluePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(GreenPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(BluePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(GreenPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	Durin::Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticModelImportBuildTests, RejectsMissingReimportSidecarWithoutChangingGraph)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticModelMissingSidecar";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
		std::filesystem::create_directories(Directory);
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TextureImportTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/Reimport/MissingSidecar");
	const Durin::FStaticModelImportPlanResult InitialPlan =
		Durin::PlanStaticModelImport({
			.SourceFile = std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/MaterialContract.gltf",
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/TextureImportTests/Models/MaterialContract.gltf"}});
	ASSERT_TRUE(InitialPlan) << InitialPlan.Message;
	const Durin::FStaticModelImportExecutionResult Initial =
		Durin::ExecuteStaticModelImport(InitialPlan.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string Fingerprint =
		Initial.StaticMesh->GetImportManifest().DependencyFingerprint;
	Durin::DMaterialInstance* const FirstMaterial = Initial.Materials.front();
	const Durin::FGuid FirstSlot =
		Initial.StaticMesh->GetMaterialSlot(0)->SlotId;

	const std::filesystem::path MissingSidecar =
		Root / "Project/SourceAssets/Models/Triangle.bin";
	std::error_code RemoveError;
	EXPECT_TRUE(std::filesystem::remove(MissingSidecar, RemoveError));
	EXPECT_FALSE(RemoveError);
	const Durin::FStaticModelImportPlanResult MissingPlan =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	EXPECT_FALSE(MissingPlan);
	EXPECT_EQ(
		Initial.StaticMesh->GetImportManifest().DependencyFingerprint,
		Fingerprint);
	EXPECT_EQ(
		Initial.StaticMesh->GetMaterialSlot(0)->DefaultMaterial.Get(),
		FirstMaterial);
	EXPECT_EQ(Initial.StaticMesh->GetMaterialSlot(0)->SlotId, FirstSlot);

	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	for (auto It = InitialPlan.Plan.Assets.rbegin();
		It != InitialPlan.Plan.Assets.rend(); ++It)
	{
		if (It->AssetPath == RootPath) continue;
		ASSERT_TRUE(Durin::Asset::UnloadPackage(It->AssetPath));
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	for (auto It = InitialPlan.Plan.Assets.rbegin();
		It != InitialPlan.Plan.Assets.rend(); ++It)
	{
		if (It->AssetPath == RootPath) continue;
		ASSERT_TRUE(Durin::Asset::DeleteAsset(It->AssetPath));
	}
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	Durin::Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticModelImportBuildTests, RequiresExplicitRecreateForMissingManagedTexture)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "StaticModelMissingGenerated";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
		std::filesystem::create_directories(Directory);
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/TextureImportTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	const Durin::FAssetPath RootPath =
		MakeAssetPath("/TextureImportTests/Reimport/MissingGenerated");
	const Durin::FStaticModelImportPlanResult InitialPlan =
		Durin::PlanStaticModelImport({
			.SourceFile = std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf",
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/TextureImportTests/Models/MissingGenerated.gltf"}});
	ASSERT_TRUE(InitialPlan) << InitialPlan.Message;
	const Durin::FStaticModelImportExecutionResult Initial =
		Durin::ExecuteStaticModelImport(InitialPlan.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	ASSERT_EQ(Initial.Textures.size(), 1u);
	const Durin::FAssetPath TexturePath = InitialPlan.Plan.Assets[1].AssetPath;
	const Durin::FAssetPath OtherOwnerPath =
		MakeAssetPath("/TextureImportTests/Reimport/OtherOwner");
	Durin::DMaterial* OtherOwner = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(OtherOwnerPath, OtherOwner));
	Initial.Textures[0]->SetImportOwner(OtherOwnerPath);
	const Durin::FStaticModelImportPlanResult OwnershipConflict =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	EXPECT_FALSE(OwnershipConflict);
	EXPECT_NE(OwnershipConflict.Message.find("now owned by"), std::string::npos);
	Initial.Textures[0]->SetImportOwner(RootPath);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(
		OtherOwner->GetPackage()));
	const Durin::FAssetPath MovedOldTexturePath =
		MakeAssetPath("/TextureImportTests/Reimport/DetachedOldTexture");
	ASSERT_TRUE(Durin::Asset::MoveAsset(TexturePath, MovedOldTexturePath));
	auto& MutableManifest =
		const_cast<Durin::FStaticModelImportManifest&>(
			Initial.StaticMesh->GetImportManifest());
	ASSERT_EQ(MutableManifest.Textures.size(), 1u);
	MutableManifest.Textures[0].GeneratedTexture = nullptr;

	const Durin::FStaticModelImportPlanResult Blocked =
		Durin::PlanStaticModelReimport({.StaticMesh = Initial.StaticMesh});
	EXPECT_FALSE(Blocked);
	EXPECT_NE(Blocked.Message.find("explicitly enable recreation"), std::string::npos);

	Durin::DMaterial* Incompatible = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(TexturePath, Incompatible));
	const Durin::FStaticModelImportPlanResult Collision =
		Durin::PlanStaticModelReimport({
			.StaticMesh = Initial.StaticMesh,
			.bRecreateMissingAssets = true});
	EXPECT_FALSE(Collision);
	EXPECT_NE(Collision.Message.find("collides"), std::string::npos);
	ASSERT_TRUE(Durin::Asset::DiscardUnpublishedPackage(
		Incompatible->GetPackage()));

	const Durin::FStaticModelImportPlanResult RecreatePlan =
		Durin::PlanStaticModelReimport({
			.StaticMesh = Initial.StaticMesh,
			.bRecreateMissingAssets = true});
	ASSERT_TRUE(RecreatePlan) << RecreatePlan.Message;
	const Durin::FStaticModelImportExecutionResult Recreated =
		Durin::ExecuteStaticModelImport(RecreatePlan.Plan);
	ASSERT_TRUE(Recreated) << Recreated.Message;
	ASSERT_EQ(Recreated.Textures.size(), 1u);
	EXPECT_NE(Recreated.Textures[0], Initial.Textures[0]);
	EXPECT_EQ(
		Recreated.Textures[0]->GetPackage()->GetPackagePath(),
		TexturePath.ToString());
	EXPECT_NE(
		Durin::Asset::GetAssetRegistry().FindAsset(MovedOldTexturePath),
		nullptr);

	const Durin::FAssetPath MaterialPath = InitialPlan.Plan.Assets[2].AssetPath;
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MovedOldTexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MovedOldTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	Durin::Testing::RemoveTestWorkDirectory(Root);
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

TEST(FStaticModelImportBuildTests, FailedMutationRunsItsRollback)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportFailedMutationDDC");
	const Durin::FAssetPath AssetPath =
		MakeAssetPath("/TextureImportTests/StaticModelImport/FailedMutation");
	int LoadedState = 7;
	int RollbackCount = 0;

	Durin::FMultiAssetImportTransaction Transaction;
	Transaction.AddTexture(MakeEmbeddedRequest(
		AssetPath.ToString(),
		"/TextureImportTests/Models/Embedded/FailedMutation.png",
		true));
	Transaction.AddLoadedObjectMutation(
		[&](std::string& OutError) {
			LoadedState = 99;
			OutError = "Injected partial mutation failure.";
			return false;
		},
		[&] {
			LoadedState = 7;
			++RollbackCount;
		});

	const Durin::FImportTransactionResult Result = Transaction.Execute();
	EXPECT_FALSE(Result);
	EXPECT_EQ(LoadedState, 7);
	EXPECT_EQ(RollbackCount, 1);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AssetPath), nullptr);
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	EXPECT_FALSE(std::filesystem::exists(PackageFile("FailedMutation")));
	EXPECT_FALSE(std::filesystem::exists(SourceFile("FailedMutation.png")));
	EXPECT_EQ(CountDerivedDataObjects(), 0u);
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
