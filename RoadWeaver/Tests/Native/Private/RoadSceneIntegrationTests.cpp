#include "RoadNet/RoadNetActor.h"
#include "RoadNet/RoadNetBuilder.h"
#include "Components/SplineMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshSource.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageInspection.h"
#include "Engine/Level.h"
#include "Actors/CameraActor.h"
#if DURIN_WITH_EDITOR
#include "AssetForge/Builtins/StaticMeshImportData.h"
#endif
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Asset/Asset.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCompilingManager.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include <gtest/gtest.h>

using namespace Durin;
using namespace Durin::RoadNet;

namespace
{
	auto Definition() -> FDefinition
	{
		FRoadNetBuilder Builder;
		FRoad Road;
		Road.StartNodeId = Builder.AddNode("A", {0, 0, 0});
		Road.EndNodeId = Builder.AddNode("B", {100, 0, 0});
		Road.ReferenceLine.SetPoints({FSplinePoint({0, 0, 0}), FSplinePoint({100, 0, 0})});
		FLaneSection Section;
		Section.EndDistanceMeters = 100;
		Section.Lanes = {{.Index = 1}, {.Index = -1, .Direction = ELaneDirection::AgainstReferenceLine}};
		Road.LaneSections = {Section};
		Builder.AddRoad(Road);
		return Builder.TakeDefinition();
	}
}

TEST(RoadSceneIntegration, LoadedPreviewMeshFinishesCompilationBeforeConstruction)
{
	InitializeTaskScheduler(2);
	Testing::InitializeDObjectSystemForTests();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, "PendingPreviewMesh");
	FStaticMeshDecodedGeometry Geometry;
	Geometry.MaterialSlots.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	auto& Section = Geometry.Meshes.emplace_back();
	Section.Name = "Triangle";
	Section.Positions = {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
	Section.Indices = {0, 1, 2};
	Section.SourceMaterialIndex = 0;
	std::string Error;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error)) << Error;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh,
		{.Source = std::move(Source), .bPersistDerivedData = false, .bMarkPackageDirty = false}, Error)) << Error;
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	auto* Asset = NewObject<DRoadNet>(nullptr, "LoadedSceneRoad");
	ASSERT_TRUE(Asset->SetDefinition(Definition(), Error)) << Error;
	auto* Actor = NewObject<ARoadNetActor>(nullptr, "LoadedRoadPreview");
	Actor->SetPreviewMesh(Mesh);
	Actor->SetRoadNet(Asset);
	EXPECT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
	Actor->PostLoad();
	const auto Components = Actor->FindComponentsByClass<DSplineMeshComponent>();
	EXPECT_EQ(Components.size(), 1);
	if (!Components.empty())
	{
		const auto State = Components.front()->GetDerivedState();
		EXPECT_TRUE(State && State->IsValid());
	}
	Actor->SetRoadNet(nullptr);
	Actor->SetPreviewMesh(nullptr);
	Actor->BeginDestroy();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
}

TEST(RoadSceneIntegration, ReuseFailureCleanupRecoveryAndDetach)
{
	Testing::InitializeDObjectSystemForTests();
	auto* Asset = NewObject<DRoadNet>(nullptr, "SceneRoad");
	std::string Error;
	ASSERT_TRUE(Asset->SetDefinition(Definition(), Error)) << Error;
	auto* Actor = NewObject<ARoadNetActor>(nullptr, "RoadPreview");
	auto* Mesh = DStaticMesh::CreateDebugTriangle();
	Actor->SetPreviewMesh(Mesh);
	Actor->SetRoadNet(Asset);
	ASSERT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
	auto Components = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Components.size(), 1);
	ASSERT_TRUE(Actor->RequestNativeReconstruction());
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>()[0], Components[0]);
	Actor->SetPreviewMesh(nullptr);
	EXPECT_EQ(Actor->GetGenerationState(), "Error");
	EXPECT_FALSE(Actor->GetDiagnostic().empty());
	EXPECT_TRUE(Actor->FindComponentsByClass<DSplineMeshComponent>().empty());
	EXPECT_TRUE(Actor->GetAlignments().empty());
	EXPECT_TRUE(Components[0]->IsPendingKill());
	EXPECT_FALSE(Actor->RequestNativeReconstruction());
	Actor->SetPreviewMesh(Mesh);
	ASSERT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
	EXPECT_TRUE(Actor->GetDiagnostic().empty());
	ASSERT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 1);
	EXPECT_NE(Actor->FindComponentsByClass<DSplineMeshComponent>()[0], Components[0]);
	auto Changed = Asset->GetDefinition();
	const auto BeforeMutation = Actor->GetAlignments().front();
	Changed.Roads[0].Name = "Revised";
	Changed.Roads[0].LaneSections[0].Lanes[0].WidthMeters = 5;
	ASSERT_TRUE(Asset->SetDefinition(Changed, Error));
	ASSERT_FALSE(Actor->GetAlignments().empty());
	EXPECT_NE(Actor->GetAlignments().front(), BeforeMutation);
	FRoadSample UpdatedSample;
	ASSERT_TRUE(Actor->GetAlignments().front()->Sample(0, UpdatedSample, Error));
	EXPECT_EQ(UpdatedSample.Lanes.front().MaximumMeters, 5);
	Actor->SetRoadNet(nullptr);
	EXPECT_EQ(Actor->GetGenerationState(), "Empty");
	EXPECT_TRUE(Actor->FindComponentsByClass<DSplineMeshComponent>().empty());
	Actor->BeginDestroy();
}

TEST(RoadSceneIntegration, AssetRoundTripPreservesIdsAndRejectedMutationDirtyState)
{
	Testing::InitializeDObjectSystemForTests();
	const auto Root = Testing::CreateTestFixtureDirectory("RoadPackages");
	Testing::RegisterMountPointForTests("/RoadTests/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/RoadTests/Network", Path));
	DRoadNet* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	auto Value = Definition();
	std::string Error;
	ASSERT_TRUE(Asset->SetDefinition(Value, Error));
	ASSERT_TRUE(SavePackage(Asset->GetPackage()));
	EXPECT_FALSE(Asset->GetPackage()->IsDirty());
	auto Invalid = Value;
	Invalid.Roads[0].LaneSections[0].Lanes[1].Id = Invalid.Roads[0].LaneSections[0].Lanes[0].Id;
	EXPECT_FALSE(Asset->SetDefinition(Invalid, Error));
	EXPECT_FALSE(Asset->GetPackage()->IsDirty());
	EXPECT_EQ(Asset->GetRoads()[0].LaneSections[0].Lanes[1].Id, Value.Roads[0].LaneSections[0].Lanes[1].Id);
	ASSERT_TRUE(UnloadPackage(Path));
	DObject* Loaded = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Loaded));
	Asset = Cast<DRoadNet>(Loaded);
	ASSERT_NE(Asset, nullptr);
	EXPECT_EQ(Asset->GetRoads()[0].Id, Value.Roads[0].Id);
	EXPECT_EQ(Asset->GetRoads()[0].ReferenceLine.GetPoints(), Value.Roads[0].ReferenceLine.GetPoints());
	EXPECT_EQ(Asset->GetSchemaVersion(), RoadNetSchemaVersion);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(RoadSceneIntegration, ReflectedDraftRejectsBeforeApplyAndReplayNotifies)
{
	auto* Asset = NewObject<DRoadNet>(nullptr, "ReflectedRoad");
	auto* Draft = NewObject<DRoadNet>(nullptr, "RoadDraft");
	std::string Error;
	ASSERT_TRUE(Asset->SetDefinition(Definition(), Error));
	auto* Property = Asset->GetClass()->FindPropertyByName(FName("Definition"));
	ASSERT_NE(Property, nullptr);
	auto* Candidate = Property->ContainerPtrToValuePtr<FDefinition>(Draft);
	*Candidate = Asset->GetDefinition();
	Candidate->Roads[0].LaneSections[0].StartDistanceMeters = 1;
	FPropertyEditProposal Proposal{.MemberProperty = Property, .LeafProperty = Property,
		.DraftRootProperty = Property, .DraftRootContainer = Draft, .DraftLeafContainer = Draft};
	int Notifications = 0;
	const auto Listener = Asset->AddMutationListener([&] { ++Notifications; });
	EXPECT_FALSE(Asset->PreEditChangeProperty(Proposal, Error));
	EXPECT_EQ(Notifications, 0);
	EXPECT_EQ(Asset->GetRoads()[0].LaneSections[0].StartDistanceMeters, 0);
	for (auto Origin : {EPropertyChangeOrigin::Undo, EPropertyChangeOrigin::Redo})
	{
		Proposal.Origin = Origin;
		EXPECT_FALSE(Asset->PreEditChangeProperty(Proposal, Error));
	}
	*Candidate = Asset->GetDefinition();
	ASSERT_TRUE(Asset->PreEditChangeProperty(Proposal, Error));
	Asset->PostEditChangeProperty({.MemberProperty = Property, .Origin = EPropertyChangeOrigin::Undo});
	EXPECT_EQ(Notifications, 1);
	Asset->PostEditChangeProperty({.MemberProperty = Property, .Origin = EPropertyChangeOrigin::Redo});
	EXPECT_EQ(Notifications, 2);
	Asset->RemoveMutationListener(Listener);
}

#if DURIN_WITH_EDITOR
TEST(RoadSceneIntegration, CheckedInRoadLevelLoadsAndSurvivesGC)
{
	InitializeTaskScheduler(2);
	Testing::InitializeDObjectSystemForTests();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	const auto Project = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
	const auto Root = Testing::CreateTestFixtureDirectory("CheckedInRoadContent");
	std::filesystem::copy(Project / "Content", Root, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
	Testing::RegisterMountPointForTests("/Game/", Root.generic_string() + "/");
	Testing::RegisterMountPointForTests("/Engine/", (Project.parent_path() / "Engine/Content").generic_string() + "/", true, false);
	ACameraActor::StaticClass();
	AssetForge::Builtins::DStaticMeshImportData::StaticClass();
	ASSERT_TRUE(RefreshAssetRegistry());
	for (const auto& File : {Root / "Levels/L_RoadNet.dasset", Project.parent_path() / "Engine/Content/Models/SplineBox.dasset"})
	{
		FAssetPackageInspection Info;
		ASSERT_TRUE(InspectAssetPackage(File.generic_string(), Info));
		for (const auto& Object : Info.Objects)
			EXPECT_NE(FindClassByQualifiedName(FName(Object.ClassName)), nullptr) << Object.ClassName;
	}
	FObjectPath ObjectPath;
	ASSERT_TRUE(FObjectPath::TryCreate("/Game/Levels/L_RoadNet.NewLevel", ObjectPath));
	DObject* Loaded = nullptr;
	const auto Result = LoadObject(ObjectPath, Loaded);
	ASSERT_TRUE(Result) << Result.Message;
	auto* Level = Cast<DLevel>(Loaded);
	ASSERT_NE(Level, nullptr);
	std::string Error;
	int RoadCount = 0;
	for (const auto& Item : Level->GetActors())
		if (auto* Actor = Cast<ARoadNetActor>(Item.Get()))
		{
			++RoadCount;
			EXPECT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
			ASSERT_NE(Actor->GetRoadNet(), nullptr);
			EXPECT_TRUE(ValidateDefinition(Actor->GetRoadNet()->GetDefinition(), Error)) << Error;
		}
	EXPECT_GT(RoadCount, 0);
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	CollectGarbage();
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	{ const auto Saved = SavePackage(Level->GetPackage()); ASSERT_TRUE(Saved) << Saved.Message; }
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
}

#endif
