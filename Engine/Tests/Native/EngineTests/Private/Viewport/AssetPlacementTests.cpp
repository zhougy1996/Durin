#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Actors/StaticMeshActor.h"
#include "Asset/Load.h"
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
	auto Result = Placement->PlaceAsset({Path, *Level}, {false});
	ASSERT_FALSE(Result);
	EXPECT_TRUE(Result.error().contains("No placement factory")) << Result.error();
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
		auto ConfigureActor(DObject&, AActor&) const -> void override {}
	};

	Tests::FActorEditorFixture Editor;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	ASSERT_TRUE(RefreshAssetRegistry());
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FSoftObjectPath Path;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(DefaultMaterialObjectPath, Path));
	FAssetPlacementInfo Info{Path, *Editor.GetWorld()->GetCurrentLevel()};
	FTestMaterialFactory Factory;
	auto Registration = RegisterModularFeature(Factory);
	auto Placed = Placement->PlaceAsset(Info, {false});
	ASSERT_TRUE(Placed) << Placed.error();
	ASSERT_TRUE(Info.PreferredLevel.ContainsActor(*Placed));
	Registration.Reset();
	const auto HistorySize = Editor.GetTransactions()->GetQueueLength();
	auto Unsupported = Placement->PlaceAsset(Info, {false});
	ASSERT_FALSE(Unsupported);
	EXPECT_TRUE(Unsupported.error().contains("No placement factory"));
	EXPECT_EQ(Editor.GetTransactions()->GetQueueLength(), HistorySize);
}

TEST(FAssetPlacementTests, RegisteredMeshFactoryPlacesAtRequestedTransformAndSupportsUndo)
{
	using namespace Durin;
	Tests::FActorEditorFixture Editor;
	Testing::RegisterMountPointForTests("/PlacementTests/", Testing::GetTestWorkDirectory().generic_string());
	auto* Placement = Editor.GetEditor()->GetEditorSubsystem<DPlacementSubsystem>();
	ASSERT_NE(Placement, nullptr);
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/PlacementTests/PlacementMesh", PackagePath));
	DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(PackagePath, Mesh));
	const auto Path = Testing::MakePackageLeafAssetObjectPathForTests(PackagePath);
	FAssetPlacementInfo Info{Path, *Editor.GetWorld()->GetCurrentLevel()};
	Info.FinalizedTransform.Translation = FVector3(3.0, 7.0, 12.0);
	const auto Registration = LevelEditor::RegisterStaticMeshActorFactory();
	ASSERT_FALSE(Placement->PlaceAsset(Info, {true}));
	TStrongObjectPtr<DLevel> Foreign(NewObject<DLevel>(nullptr, "ForeignPlacementLevel"));
	ASSERT_FALSE(Placement->PlaceAsset({Path, *Foreign}, {false}));
	EXPECT_FALSE(Editor.GetTransactions()->CanUndo());
	auto Result = Placement->PlaceAsset(Info, {false});
	ASSERT_TRUE(Result) << Result.error();
	auto* Actor = Cast<AStaticMeshActor>(*Result);
	ASSERT_NE(Actor, nullptr);
	auto* Component = Actor->GetStaticMeshComponent();
	EXPECT_EQ(Component->GetStaticMesh(), Mesh);
	EXPECT_EQ(Component->GetWorldLocation(), Info.FinalizedTransform.Translation);
	ASSERT_TRUE(Editor.GetTransactions()->Undo());
	EXPECT_FALSE(Info.PreferredLevel.ContainsActor(Actor));
	ASSERT_TRUE(Editor.GetTransactions()->Redo());
	EXPECT_TRUE(Info.PreferredLevel.ContainsActor(Actor));
}
