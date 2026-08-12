#include "Components/SplineMeshComponent.h"

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StandardAssetImportProviders.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	auto MakeComponentWithMesh() -> std::pair<DSplineMeshComponent*, DStaticMesh*>
	{
		auto* Component = NewObject<DSplineMeshComponent>(nullptr, "SplineMeshComponent");
		auto* Mesh = DStaticMesh::CreateDebugTriangle();
		EXPECT_NE(Component, nullptr);
		EXPECT_NE(Mesh, nullptr);
		Component->SetStaticMesh(Mesh);
		return {Component, Mesh};
	}
}

TEST(FSplineMeshComponentTests, DefaultObjectPublishesDiagnosticStateAndReflectsAuthoredFields)
{
	auto* Component = NewObject<DSplineMeshComponent>(nullptr, "DefaultSplineMeshComponent");
	ASSERT_NE(Component, nullptr);
	const auto State = Component->GetDerivedState();
	ASSERT_NE(State, nullptr);
	EXPECT_EQ(State->Status, ESplineMeshDerivedStateStatus::NoStaticMesh);
	EXPECT_EQ(Component->GetDeformationRevision(), 0u);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("StaticMesh")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("SplineMeshParams")), nullptr);
	EXPECT_NE(Component->GetClass()->FindPropertyByName(FName("OverrideMaterials")), nullptr);
}

TEST(FSplineMeshComponentTests, PublishesNormalizedExactLOD0AndConservativeBounds)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const auto Initial = Component->GetDerivedState();
	ASSERT_TRUE(Initial && Initial->IsValid()) << (Initial ? Initial->Diagnostic : "missing state");
	EXPECT_EQ(Initial->DeformedLOD0Positions.size(), 3u);
	EXPECT_EQ(Initial->LOD0Indices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_NE(Initial->EditorAcceleration, nullptr);
	EXPECT_NE(Initial->CollisionInputIdentity, 0u);
	EXPECT_EQ(Initial->SourceRenderResourceRevision, Mesh->GetRenderResourceStatus().Revision);
	EXPECT_NEAR(Initial->Params.SourceForwardMin, -0.65, 1.e-6);
	EXPECT_NEAR(Initial->Params.SourceForwardMax, 0.65, 1.e-6);
	for (const FVector3f& Position : Initial->DeformedLOD0Positions)
	{
		EXPECT_GE(Position.x, Initial->ConservativeLocalBounds.Min.x - 1.e-5);
		EXPECT_LE(Position.x, Initial->ConservativeLocalBounds.Max.x + 1.e-5);
		EXPECT_GE(Position.y, Initial->ConservativeLocalBounds.Min.y - 1.e-5);
		EXPECT_LE(Position.y, Initial->ConservativeLocalBounds.Max.y + 1.e-5);
		EXPECT_GE(Position.z, Initial->ConservativeLocalBounds.Min.z - 1.e-5);
		EXPECT_LE(Position.z, Initial->ConservativeLocalBounds.Max.z + 1.e-5);
	}

	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {0.0, 120.0, 20.0};
	Params.EndTangent = {0.0, 100.0, 0.0};
	std::string Error;
	ASSERT_TRUE(Component->SetSplineMeshParams(Params, &Error)) << Error;
	const auto Curved = Component->GetDerivedState();
	ASSERT_TRUE(Curved && Curved->IsValid());
	EXPECT_GT(Curved->DeformationRevision, Initial->DeformationRevision);
	EXPECT_NE(Curved->CollisionInputIdentity, Initial->CollisionInputIdentity);
	EXPECT_EQ(Initial->Params.EndPosition, FVector3(100.0, 0.0, 0.0));
}

TEST(FSplineMeshComponentTests, InvalidProposalPreservesAuthoredAndPublishedState)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	const FSplineMeshParams BeforeParams = Component->GetSplineMeshParams();
	const auto BeforeState = Component->GetDerivedState();
	const uint64 BeforeRevision = Component->GetDeformationRevision();
	FSplineMeshParams Invalid = BeforeParams;
	Invalid.StartPosition.x = std::numeric_limits<double>::quiet_NaN();
	std::string Error;
	EXPECT_FALSE(Component->SetSplineMeshParams(Invalid, &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Component->GetSplineMeshParams(), BeforeParams);
	EXPECT_EQ(Component->GetDerivedState(), BeforeState);
	EXPECT_EQ(Component->GetDeformationRevision(), BeforeRevision);
}

TEST(FSplineMeshComponentTests, ReflectedInvalidDraftIsRejectedBeforeMutation)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Draft = NewObject<DSplineMeshComponent>(nullptr, "SplineMeshInvalidDraft");
	DClass* Class = DSplineMeshComponent::StaticClass();
	FProperty* ParamsProperty = Class->FindPropertyByName(FName("SplineMeshParams"));
	ASSERT_NE(ParamsProperty, nullptr);
	auto* DraftParams = ParamsProperty->ContainerPtrToValuePtr<FSplineMeshParams>(Draft);
	*DraftParams = Component->GetSplineMeshParams();
	DraftParams->EndRollRadians = std::numeric_limits<double>::infinity();
	FPropertyEditProposal Proposal{
		.MemberProperty = ParamsProperty,
		.LeafProperty = ParamsProperty,
		.DraftRootProperty = ParamsProperty,
		.DraftRootContainer = Draft,
		.DraftLeafContainer = Draft};
	std::string Error;
	EXPECT_FALSE(Component->PreEditChangeProperty(Proposal, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_TRUE(Math::IsFinite(Component->GetSplineMeshParams().EndPosition));
	EXPECT_TRUE(std::isfinite(Component->GetSplineMeshParams().EndRollRadians));
}

TEST(FSplineMeshComponentTests, DuplicateRebuildsIndependentEquivalentSnapshot)
{
	auto [Source, Mesh] = MakeComponentWithMesh();
	FSplineMeshParams Params = Source->GetSplineMeshParams();
	Params.EndPosition = {75.0, 25.0, 10.0};
	ASSERT_TRUE(Source->SetSplineMeshParams(Params));
	std::string Error;
	auto* Duplicate = Cast<DSplineMeshComponent>(
		DuplicateObjectGraph(Source, nullptr, "SplineMeshDuplicate", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	EXPECT_EQ(Duplicate->GetStaticMesh(), Mesh);
	EXPECT_EQ(Duplicate->GetSplineMeshParams(), Source->GetSplineMeshParams());
	const auto SourceState = Source->GetDerivedState();
	const auto DuplicateState = Duplicate->GetDerivedState();
	ASSERT_TRUE(SourceState && DuplicateState && DuplicateState->IsValid());
	EXPECT_NE(SourceState, DuplicateState);
	EXPECT_EQ(DuplicateState->Params, SourceState->Params);
	EXPECT_EQ(DuplicateState->DeformedLOD0Positions, SourceState->DeformedLOD0Positions);
	EXPECT_EQ(DuplicateState->LOD0Indices, SourceState->LOD0Indices);
}

TEST(FSplineMeshComponentTests, MaterialOverridesUseStaticMeshSlotRules)
{
	auto [Component, Mesh] = MakeComponentWithMesh();
	auto* Material = NewObject<DMaterial>(nullptr, "SplineMeshMaterial");
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(Component->GetNumMaterials(), 1u);
	EXPECT_TRUE(Component->SetMaterial(0, Material));
	EXPECT_EQ(Component->GetMaterial(), Material);
	EXPECT_FALSE(Component->SetMaterial(1, Material));
	EXPECT_TRUE(Component->ResetMaterial(0));
	EXPECT_EQ(Component->GetMaterial(), nullptr);
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
}

TEST(FSplineMeshComponentTests, LevelPackageRoundTripsAuthoredFieldsAndRebuildsDerivedState)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "SplineMeshComponentAssets";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Testing::RemoveTestWorkDirectory(Root);
		PathUtilities::RegisterMountPointForTests("/SplineMeshComponentTests/", Root.generic_string() + "/");
	}
	FAssetPath Path;
	ASSERT_TRUE(FAssetPath::TryCreate("/SplineMeshComponentTests/RoundTrip", Path));
	DLevel* Level = nullptr;
	std::string ProviderError;
	ASSERT_TRUE(RegisterStandardAssetImportProviders(ProviderError)) << ProviderError;
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	FStaticMeshImportResult MeshImport = DStaticMesh::ImportAsset(
		Source.generic_string(), "/SplineMeshComponentTests/SourceMesh");
	ASSERT_TRUE(MeshImport) << MeshImport.Message;
	ASSERT_TRUE(Asset::CreateAsset(Path, Level));
	auto* Actor = Level->SpawnActor<AActor>("SplineMeshActor");
	auto* Component = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(
		DSplineMeshComponent::StaticClass(), FName("SplineMesh")));
	ASSERT_NE(Component, nullptr);
	Component->SetStaticMesh(MeshImport.Asset);
	FSplineMeshParams Params = Component->GetSplineMeshParams();
	Params.EndPosition = {80.0, 30.0, 15.0};
	Params.EndRollRadians = 0.75;
	Params.EndScale = {2.0, 0.5};
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	ASSERT_TRUE(Asset::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Asset::UnloadPackage(Path));

	DObject* LoadedObject = nullptr;
	ASSERT_TRUE(Asset::LoadAsset(Path, LoadedObject));
	auto* LoadedLevel = Cast<DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	auto* Loaded = LoadedLevel->FindActorByName("SplineMeshActor")
		->FindComponentByClass<DSplineMeshComponent>();
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetSplineMeshParams().EndPosition, Params.EndPosition);
	EXPECT_DOUBLE_EQ(Loaded->GetSplineMeshParams().EndRollRadians, Params.EndRollRadians);
	EXPECT_EQ(Loaded->GetSplineMeshParams().EndScale, Params.EndScale);
	const auto State = Loaded->GetDerivedState();
	ASSERT_TRUE(State && State->IsValid()) << (State ? State->Diagnostic : "missing state");
	EXPECT_EQ(State->Params, Loaded->GetSplineMeshParams());
	EXPECT_EQ(State->DeformedLOD0Positions.size(), 3u);
	EXPECT_TRUE(Asset::UnloadPackage(Path));
	UnregisterStandardAssetImportProviders();
}
