#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Actors/StaticMeshActor.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "NativeAssetTestSupport.h"
#include "AssetRegistry/Scan.h"
#include "Components/StaticMeshComponent.h"
#include "Factories/ActorFactory.h"
#include "Materials/DefaultMaterialService.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Operations/ActorFactoryStaticMesh.h"
#include "StaticMesh/StaticMesh.h"
#include "Subsystems/PlacementSubsystem.h"
#include <gtest/gtest.h>

TEST(FAssetPlacementTests, BuiltInFactoriesLeaveCubemapsUnplaced)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	ASSERT_TRUE(RefreshAssetRegistry());
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/Engine/Renderer/DefaultSkyCube.DefaultSkyCube", Path));
	const auto Registration = LevelEditor::RegisterStaticMeshActorFactory();
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	const auto ActorCount = Level->GetActors().size();
	const auto* ResidentBefore = FindResidentPackage(Path.GetPackagePath());
	std::string Error = "previous error";
	const auto Cube = FindAssetExact(Path.GetAssetPath());
	ASSERT_TRUE(Cube);
	EXPECT_TRUE(Placement->PlaceAsset({*Cube.Asset, Level, {false}}, &Error).empty());
	EXPECT_TRUE(Error.empty());
	EXPECT_EQ(Level->GetActors().size(), ActorCount);
	EXPECT_EQ(FindResidentPackage(Path.GetPackagePath()), ResidentBefore);
	EXPECT_FALSE(Editor.GetTransactions()->CanUndo());
}

TEST(FAssetPlacementTests, ModuleFactoryIsUsedOnlyWhileRegistered)
{
	using namespace Durin;
	class FTestMaterialFactory final : public FActorFactory
	{
	public:
		FTestMaterialFactory() : FActorFactory(DMaterial::StaticClass(), AActor::StaticClass()) {}
		auto CanPlaceElementsFromAssetData(const FAssetData& Data) const -> bool override
		{
			return Data.PrimaryAssetId.PrimaryAssetName == FName("PlacementTest")
				&& FActorFactory::CanPlaceElementsFromAssetData(Data);
		}
		auto ConfigureActor(DObject&, AActor&) const -> void override {}
	};

	Tests::FActorEditorFixture Editor;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	ASSERT_TRUE(RefreshAssetRegistry());
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(DefaultMaterialObjectPath, Path));
	const auto Material = FindAssetExact(Path.GetAssetPath());
	ASSERT_TRUE(Material);
	FAssetPlacementInfo Info{*Material.Asset, Editor.GetWorld()->GetCurrentLevel(), {false}};
	FTestMaterialFactory Factory;
	auto Registration = RegisterModularFeature(Factory);
	EXPECT_TRUE(Placement->PlaceAsset(Info).empty());
	ASSERT_TRUE(FPrimaryAssetId::TryParse("Material:PlacementTest", Info.AssetToPlace.PrimaryAssetId));
	auto Placed = Placement->PlaceAsset(Info);
	ASSERT_EQ(Placed.size(), 1u);
	ASSERT_TRUE(Info.PreferredLevel.Get()->ContainsActor(Placed.front().Get()));
	Registration.Reset();
	const auto HistorySize = Editor.GetTransactions()->GetQueueLength();
	EXPECT_TRUE(Placement->PlaceAsset(Info).empty());
	EXPECT_EQ(Editor.GetTransactions()->GetQueueLength(), HistorySize);
}

TEST(FAssetPlacementTests, RegisteredMeshFactoryPlacesAtRequestedTransformAndSupportsUndo)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	const auto Root = Testing::CreateTestFixtureDirectory("PlacementMesh");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/PlacementTests/", .Owner = EMountOwner::Test,
		.Root = Root.generic_string(), .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/PlacementTests/PlacementMesh", PackagePath));
	DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Mesh));
	const auto Path = Testing::MakePackageLeafAssetObjectPathForTests(PackagePath);
	FAssetData Data{.AssetPath = Path.GetAssetPath(), .AssetClassName = Mesh->GetClass()->GetQualifiedName().ToString()};
	FAssetPlacementInfo Info{Data, Editor.GetWorld()->GetCurrentLevel(), {false}};
	Info.FinalizedTransform.Translation = FVector3(3.0, 7.0, 12.0);
	const auto Registration = LevelEditor::RegisterStaticMeshActorFactory();
	std::string Error;
	EXPECT_TRUE(Placement->PlaceAsset({Data, Info.PreferredLevel, {true}}, &Error).empty());
	EXPECT_TRUE(Error.contains("read-only"));
	TStrongObjectPtr<DLevel> Foreign(NewObject<DLevel>(nullptr, "ForeignPlacementLevel"));
	EXPECT_TRUE(Placement->PlaceAsset({Data, Foreign.Get(), {false}}, &Error).empty());
	EXPECT_TRUE(Error.contains("current editor level"));
	EXPECT_FALSE(Editor.GetTransactions()->CanUndo());
	auto Result = Placement->PlaceAsset(Info, &Error);
	ASSERT_EQ(Result.size(), 1u) << Error;
	EXPECT_TRUE(Error.empty());
	auto* Actor = Cast<AStaticMeshActor>(Result.front().Get());
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetStaticMeshComponent();
	EXPECT_EQ(Component->GetStaticMesh(), Mesh);
	EXPECT_EQ(Component->GetWorldLocation(), Info.FinalizedTransform.Translation);
	ASSERT_TRUE(Editor.GetTransactions()->Undo());
	EXPECT_FALSE(Info.PreferredLevel.Get()->ContainsActor(Actor));
	ASSERT_TRUE(Editor.GetTransactions()->Redo());
	EXPECT_TRUE(Info.PreferredLevel.Get()->ContainsActor(Actor));
}

TEST(FAssetPlacementTests, RedirectorDispatchesWithDestinationMetadata)
{
	using namespace Durin;
	class FRedirectTargetFactory final : public FActorFactory
	{
	public:
		FRedirectTargetFactory() : FActorFactory(DMaterial::StaticClass(), AActor::StaticClass()) {}
		auto ConfigureActor(DObject&, AActor&) const -> void override {}
	};

	Tests::FActorEditorFixture Editor;
	const auto Root = Testing::CreateTestFixtureDirectory("PlacementRedirect");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/PlacementRedirect/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	FPackagePath Target, Alias;
	ASSERT_TRUE(FPackagePath::TryCreate("/PlacementRedirect/Target", Target));
	ASSERT_TRUE(FPackagePath::TryCreate("/PlacementRedirect/Alias", Alias));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Target, Material));
	FAssetWriteResult Write;
	ASSERT_TRUE(SavePackage(Material->GetPackage(), {}, &Write)) << Write.Error->ToString();
	DAssetRedirector* Redirector = nullptr;
	const auto Created = Testing::CreateAssetRedirectorForTests(Alias, Target, Redirector);
	ASSERT_TRUE(Created) << Created.Error->ToString();
	ASSERT_TRUE(SavePackage(Redirector->GetPackage(), {}, &Write)) << Write.Error->ToString();
	const auto AliasData = FindAssetExact(Testing::MakePackageLeafAssetObjectPathForTests(Alias).GetAssetPath());
	ASSERT_TRUE(AliasData);
	ASSERT_TRUE(AliasData->IsRedirector());
	FRedirectTargetFactory Factory;
	auto Registration = RegisterModularFeature(Factory);
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	std::string Error;
	auto Placed = Placement->PlaceAsset({*AliasData.Asset, Editor.GetWorld()->GetCurrentLevel(), {false}}, &Error);
	ASSERT_EQ(Placed.size(), 1u) << Error;
	ASSERT_NE(Placed.front().Get(), nullptr);
	EXPECT_EQ(Placed.front().Get()->GetName(), "Target");
}

TEST(FAssetPlacementTests, BatchCollectsMultipleActorsPerAssetAndUndoesAsOneEdit)
{
	using namespace Durin;
	class FPairFactory final : public FActorFactory
	{
	public:
		FPairFactory() : FActorFactory(DMaterial::StaticClass(), AStaticMeshActor::StaticClass()) {}
		auto ConfigureActor(DObject&, AActor&) const -> void override {}
		auto PlaceAsset(DEditorActorSubsystem& Editor, const FAssetPlacementInfo& Info, DObject& Asset,
			std::string* OutError) const -> std::vector<TWeakObjectPtr<AActor>> override
		{
			auto Pair = FActorFactory::PlaceAsset(Editor, Info, Asset, OutError);
			Pair.append_range(FActorFactory::PlaceAsset(Editor, Info, Asset, OutError));
			return Pair;
		}
	};
	Tests::FActorEditorFixture Editor;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	ASSERT_TRUE(RefreshAssetRegistry());
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FPairFactory Factory;
	auto Registration = RegisterModularFeature(Factory);
	FSoftObjectPath Path, Cube;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(DefaultMaterialObjectPath, Path));
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/Engine/Renderer/DefaultSkyCube.DefaultSkyCube", Cube));
	auto* Level = Editor.GetWorld()->GetCurrentLevel();
	const auto MaterialData = FindAssetExact(Path.GetAssetPath());
	const auto CubeData = FindAssetExact(Cube.GetAssetPath());
	ASSERT_TRUE(MaterialData);
	ASSERT_TRUE(CubeData);
	FAssetPlacementInfo First{*MaterialData.Asset, Level, {false}}, Last = First;
	Last.FinalizedTransform.Translation = FVector3(4.0, 5.0, 6.0);
	const std::array Infos{First, FAssetPlacementInfo{*CubeData.Asset, Level, {false}},
		FAssetPlacementInfo{*MaterialData.Asset, Level, {true}}, Last};
	std::string Error;
	const auto Placed = Placement->PlaceAssets(Infos, &Error);
	ASSERT_EQ(Placed.size(), 4u) << Error;
	EXPECT_TRUE(Error.contains("read-only"));
	EXPECT_FALSE(Error.contains(Cube.ToString()));
	for (size_t Index = 0; Index < Placed.size(); ++Index)
	{
		auto* Actor = Placed[Index].Get();
		ASSERT_NE(Actor, nullptr);
		EXPECT_TRUE(Level->ContainsActor(Actor));
		EXPECT_EQ(Actor->GetActorTransform().Translation,
			(Index < 2 ? First : Last).FinalizedTransform.Translation);
	}
	ASSERT_EQ(Editor.GetTransactions()->GetQueueLength(), 1u);
	ASSERT_TRUE(Editor.GetTransactions()->Undo());
	for (const auto& Actor : Placed) EXPECT_FALSE(Level->ContainsActor(Actor.Get()));
	ASSERT_TRUE(Editor.GetTransactions()->Redo());
	for (const auto& Actor : Placed) EXPECT_TRUE(Level->ContainsActor(Actor.Get()));
}
