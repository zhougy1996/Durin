#include "ViewportTestSupport.h"
#include "Actors/SkeletalMeshActor.h"
#include "Actors/TerrainActor.h"
#include "Animation/AnimationClip.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/TerrainComponent.h"
#include "LevelEditorViewportEditing.h"
#include "SkeletalMesh/Skeleton.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Terrain/TerrainHeightmap.h"
#include "Viewport/ViewportPickingSceneIndex.h"
#include "Viewport/ViewportPickingService.h"

#include <random>

namespace
{
	struct FFakePickingState
	{
		std::unordered_map<uint64, Durin::Editor::Level::FViewportPickingBackendRequest> Requests;
		std::unordered_map<uint64, Durin::Editor::Level::FViewportPickingBackendCompletion> Completions;
		std::unordered_set<uint64> Cancelled;

		auto CompleteFirst(Durin::Editor::Level::FViewportPickTicket Ticket, double Distance = 3.0) -> void
		{
			const auto It = Requests.find(Ticket.Id);
			ASSERT_NE(It, Requests.end());
			ASSERT_FALSE(It->second.Targets.empty());
			Completions[Ticket.Id] = {
				Durin::Editor::Level::EViewportPickStatus::Completed,
				Durin::Editor::Level::FViewportPickingBackendHit{It->second.Targets.front().Token, Distance, 0},
			};
		}
	};

	class FControlledPickingBackend final : public Durin::Editor::Level::IViewportPickingBackend
	{
	public:
		explicit FControlledPickingBackend(std::shared_ptr<FFakePickingState> InState) : State(std::move(InState)) {}

		auto Submit(Durin::Editor::Level::FViewportPickingBackendRequest Request) -> Durin::Editor::Level::FViewportPickingBackendCompletion override
		{
			State->Requests.emplace(Request.Ticket.Id, std::move(Request));
			return {Durin::Editor::Level::EViewportPickStatus::Pending, std::nullopt};
		}

		auto Poll(Durin::Editor::Level::FViewportPickTicket Ticket) -> Durin::Editor::Level::FViewportPickingBackendCompletion override
		{
			const auto It = State->Completions.find(Ticket.Id);
			return It == State->Completions.end()
				? Durin::Editor::Level::FViewportPickingBackendCompletion{Durin::Editor::Level::EViewportPickStatus::Pending, std::nullopt}
				: It->second;
		}

		auto Cancel(Durin::Editor::Level::FViewportPickTicket Ticket) -> void override { State->Cancelled.insert(Ticket.Id); }

	private:
		std::shared_ptr<FFakePickingState> State;
	};

	struct FPickingFixture
	{
		Durin::DWorld* World = nullptr;
		Durin::DLevel* Level = nullptr;
		Durin::AStaticMeshActor* Actor = nullptr;
		Durin::Editor::Level::FLevelEditorViewportClient Client;
		Durin::FSceneView View;

		FPickingFixture()
		{
			InitializeDObjectSystem();
			World = Durin::NewObject<Durin::DWorld>(nullptr, "PickingContractWorld");
			Level = Durin::NewObject<Durin::DLevel>(World, "PickingContractLevel");
			expect_true(World->SetCurrentLevel(Level));
			Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(Level);
			Actor = Level->SpawnActor<Durin::AStaticMeshActor>("Target");
			expect_ne(Actor, nullptr);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->GetStaticMeshComponent()->SetWorldLocation(
				Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 3.0);
			Client.InitializeForLevel(Level);
			expect_true(Client.BuildViewMatrices(800, 600, View));
		}

		~FPickingFixture()
		{
			Durin::MarkObjectHierarchyAsGarbage(World);
			Durin::CollectGarbage();
		}

	private:
		static auto expect_true(bool Value) -> void { if (!Value) throw std::runtime_error("fixture setup failed"); }
		template<typename T> static auto expect_ne(T* Value, std::nullptr_t) -> void
		{
			if (!Value) throw std::runtime_error("fixture setup failed");
		}
	};

	auto MatrixTransform(const Durin::FMatrix& Matrix) -> Durin::FSkeletonTransform
	{
		Durin::FSkeletonTransform Result;
		Durin::FVector4* Rows[] = {&Result.Row0, &Result.Row1, &Result.Row2, &Result.Row3};
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Column = 0; Column < 4; ++Column)
				(*Rows[Row])[Column] = Matrix[Column][Row];
		Result.CanonicalizeFloat32();
		return Result;
	}

	struct FSkeletalPickingFixture
	{
		Durin::DWorld* World = nullptr;
		Durin::DLevel* Level = nullptr;
		Durin::DSkeleton* Skeleton = nullptr;
		Durin::DSkeletalMesh* Mesh = nullptr;
		Durin::ASkeletalMeshActor* Actor = nullptr;

		explicit FSkeletalPickingFixture(bool bMixedInfluences = true)
		{
			InitializeDObjectSystem();
			World = Durin::NewObject<Durin::DWorld>(nullptr, "SkeletalPickingWorld");
			Level = Durin::NewObject<Durin::DLevel>(World, "SkeletalPickingLevel");
			if (!World->SetCurrentLevel(Level)) throw std::runtime_error("fixture level setup failed");

			Skeleton = Durin::NewObject<Durin::DSkeleton>(Level, "SkeletalPickingSkeleton");
			std::vector<Durin::FSkeletonBone> Bones;
			for (uint16 BoneIndex = 0; BoneIndex < 4; ++BoneIndex)
				Bones.push_back({.Name = Durin::FName(std::format("Bone{}", BoneIndex)),
					.ParentIndex = BoneIndex == 0 ? -1 : static_cast<int32>(BoneIndex - 1),
					.ReferenceTransform = MatrixTransform(Durin::FMatrix(1.0))});
			std::string Error;
			if (!Skeleton->InitializeCanonicalBones(std::move(Bones), Error)) throw std::runtime_error(Error);

			auto Payload = std::make_shared<Durin::FSkeletalMeshPayloadData>();
			Payload->Positions = {{-0.65f, -0.45f, 0.0f}, {0.65f, -0.45f, 0.0f}, {0.0f, 0.65f, 0.0f}};
			Payload->Normals = std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f});
			Payload->Tangents = std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f});
			Payload->UVChannels[0] = std::vector<Durin::FVector2f>(3, {0.0f, 0.0f});
			Payload->Colors = std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f));
			Payload->Indices = {0, 1, 2};
			Payload->Influences.resize(3);
			for (Durin::FSkeletalMeshVertexInfluences& Influence : Payload->Influences)
			{
				Influence.BoneIndices[0] = 3;
				Influence.Weights[0] = bMixedInfluences ? 0.75f : 1.0f;
				if (bMixedInfluences)
				{
					Influence.BoneIndices[1] = 0;
					Influence.Weights[1] = 0.25f;
					Influence.Count = 2;
				}
				else Influence.Count = 1;
			}
			Payload->LocalBounds = Durin::FBox({-0.65, -0.45, 0.0}, {0.65, 0.65, 0.0});
			Payload->Sections.push_back({.Name = Durin::FName("Body"), .FirstIndex = 0,
				.IndexCount = 3, .MinVertexIndex = 0, .MaxVertexIndex = 2,
				.MaterialSlotIndex = 0, .LocalBounds = Payload->LocalBounds});
			Payload->PaletteBoneIndices = {3, 0};
			Payload->InverseBindMatrices = {Durin::FMatrix4f(1.0f), Durin::FMatrix4f(1.0f)};

			Mesh = Durin::NewObject<Durin::DSkeletalMesh>(Level, "SkeletalPickingMesh");
			if (!Mesh->PublishBuiltProduct({.Skeleton = Skeleton,
				.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
				.MeshNodeBindTransform = MatrixTransform(Durin::FMatrix(1.0)),
				.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
				.Payload = std::move(Payload)}, Error)) throw std::runtime_error(Error);
			Actor = Level->SpawnActor<Durin::ASkeletalMeshActor>("SkeletalTarget");
			if (!Actor || !Actor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh, Error))
				throw std::runtime_error(Error.empty() ? "skeletal actor setup failed" : Error);
		}

		~FSkeletalPickingFixture()
		{
			Durin::MarkObjectHierarchyAsGarbage(World);
			Durin::CollectGarbage();
		}

		auto MakeRequest(Durin::FVector3 Origin = {0.0, 0.0, -2.0},
			Durin::FVector3 Direction = {0.0, 0.0, 1.0}) const -> Durin::Editor::Level::FViewportPickingBackendRequest
		{
			auto* Component = Actor->GetSkeletalMeshComponent();
			return {{1}, Origin, Direction, {{1, Component->GetPrimitiveSceneId(), Actor, Component,
				Component->GetPrimitiveSceneId().Value, Component->GetRegistrationGeneration()}}};
		}
	};

	auto MakeStaticRequest(Durin::AStaticMeshActor* Actor) -> Durin::Editor::Level::FViewportPickingBackendRequest
	{
		auto* Component = Actor->GetStaticMeshComponent();
		return {{1}, {0.1, 0.1, 0.0}, {0.0, 0.0, 1.0},
			{{1, Component->GetPrimitiveSceneId(), Actor, Component,
				Component->GetPrimitiveSceneId().Value, Component->GetRegistrationGeneration()}}};
	}

	auto CreateGridStaticMesh(Durin::DLevel* Level, uint32 TriangleCount) -> Durin::DStaticMesh*
	{
		const uint32 CellCount = (TriangleCount + 1) / 2;
		const uint32 Width = std::max<uint32>(1,
			static_cast<uint32>(std::ceil(std::sqrt(static_cast<double>(CellCount)))));
		const uint32 Height = (CellCount + Width - 1) / Width;
		Durin::Asset::FStaticMeshImportedData Imported;
		Imported.MaterialSlots.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
		auto& Mesh = Imported.Meshes.emplace_back();
		Mesh.Name = "PickingGrid";
		Mesh.SourceMaterialIndex = 0;
		Mesh.Positions.reserve(static_cast<size_t>(Width + 1) * (Height + 1));
		for (uint32 Y = 0; Y <= Height; ++Y)
			for (uint32 X = 0; X <= Width; ++X)
				Mesh.Positions.emplace_back(static_cast<float>(X), static_cast<float>(Y), 0.0f);
		Mesh.Indices.reserve(static_cast<size_t>(TriangleCount) * 3);
		for (uint32 Cell = 0; Cell < CellCount && Mesh.Indices.size() / 3 < TriangleCount; ++Cell)
		{
			const uint32 X = Cell % Width;
			const uint32 Y = Cell / Width;
			const uint32 A = Y * (Width + 1) + X;
			const uint32 B = A + 1;
			const uint32 C = A + Width + 1;
			const uint32 D = C + 1;
			Mesh.Indices.insert(Mesh.Indices.end(), {A, B, D});
			if (Mesh.Indices.size() / 3 < TriangleCount) Mesh.Indices.insert(Mesh.Indices.end(), {A, D, C});
		}
		auto* Result = Durin::NewObject<Durin::DStaticMesh>(Level,
			std::format("PickingGrid{}", TriangleCount));
		std::string Error;
		if (!Durin::Asset::FStaticMeshBuildOperations::BuildAndPublishImported(
			*Result, Imported,
			"viewport picking grid", Error)) throw std::runtime_error(Error);
		return Result;
	}
}

TEST(FViewportPickingContractTests, AppliesFrozenCrossFamilyOrdering)
{
	Durin::Editor::Level::FViewportPickHit Geometry{.Kind = Durin::Editor::Level::EViewportPickHitKind::SceneGeometry, .Distance = 5.0, .Priority = 0, .StableTieKey = 20};
	Durin::Editor::Level::FViewportPickHit Visualization{.Kind = Durin::Editor::Level::EViewportPickHitKind::EditorVisualization, .Distance = 4.0, .Priority = 100, .StableTieKey = 10};
	EXPECT_TRUE(Durin::Editor::Level::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.Distance = 6.0;
	EXPECT_FALSE(Durin::Editor::Level::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.bDepthIndependent = true;
	EXPECT_TRUE(Durin::Editor::Level::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.bDepthIndependent = false;
	Visualization.Distance = Geometry.Distance;
	EXPECT_FALSE(Durin::Editor::Level::IsViewportPickHitPreferred(Visualization, Geometry));
	Durin::Editor::Level::FViewportPickHit Stable = Geometry;
	Stable.StableTieKey = 5;
	EXPECT_TRUE(Durin::Editor::Level::IsViewportPickHitPreferred(Stable, Geometry));
}

TEST(FViewportPickingContractTests, RejectsOutsideViewAndPreservesExactPrimitiveIdentity)
{
	FPickingFixture Fixture;
	const Durin::Editor::Level::FViewportPickSubmission Outside = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {-1.0f, 300.0f});
	EXPECT_TRUE(Outside.Ticket);
	EXPECT_EQ(Outside.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Invalid);

	const Durin::Editor::Level::FViewportPickSubmission Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Kind, Durin::Editor::Level::EViewportPickHitKind::SceneGeometry);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(Pick.Completion.Hit->Component.Get(), Fixture.Actor->GetStaticMeshComponent());
	EXPECT_EQ(Pick.Completion.Hit->PrimitiveId, Fixture.Actor->GetStaticMeshComponent()->GetPrimitiveSceneId());
}

TEST(FViewportPickingContractTests, SupportsPendingPollingCancellationAndSupersession)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	const Durin::Editor::Level::FViewportPickSubmission First = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	ASSERT_EQ(First.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Pending);
	EXPECT_EQ(Fixture.Client.PollViewportPick(First.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Pending);

	const Durin::Editor::Level::FViewportPickSubmission Second = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	EXPECT_EQ(Fixture.Client.PollViewportPick(First.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Cancelled);
	EXPECT_TRUE(State->Cancelled.contains(First.Ticket.Id));
	State->CompleteFirst(Second.Ticket);
	const Durin::Editor::Level::FViewportPickCompletion Completed = Fixture.Client.PollViewportPick(Second.Ticket);
	ASSERT_EQ(Completed.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Completed.Hit);
	EXPECT_EQ(Completed.Hit->Actor.Get(), Fixture.Actor);

	const Durin::Editor::Level::FViewportPickSubmission Third = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.CancelViewportPick(Third.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Third.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Cancelled);
}

TEST(FViewportPickingContractTests, InvalidatesRetiredTargetsButNotCameraMotion)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	const Durin::Editor::Level::FViewportPickSubmission CameraStable = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.FocusLocation({100.0, 200.0, 300.0});
	State->CompleteFirst(CameraStable.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(CameraStable.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Completed);

	const Durin::Editor::Level::FViewportPickSubmission Hidden = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	State->CompleteFirst(Hidden.Ticket);
	Fixture.Actor->SetHidden(true);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Hidden.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Invalidated);
	Fixture.Actor->SetHidden(false);

	const Durin::Editor::Level::FViewportPickSubmission Reregistered = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
	Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	State->CompleteFirst(Reregistered.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Reregistered.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Invalidated);

	const Durin::Editor::Level::FViewportPickSubmission Reset = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.InitializeForLevel(Fixture.Level);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Reset.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Invalidated);

	const Durin::Editor::Level::FViewportPickSubmission Destroyed = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	State->CompleteFirst(Destroyed.Ticket);
	ASSERT_TRUE(Fixture.Level->DestroyActor(Fixture.Actor));
	Durin::CollectGarbage();
	EXPECT_EQ(Fixture.Client.PollViewportPick(Destroyed.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Invalidated);
}

TEST(FViewportPickingContractTests, CancelsPendingWorkOnModeAndViewportExit)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	ASSERT_EQ(State->Requests.size(), 1u);
	const uint64 ModeTicket = State->Requests.begin()->first;
	Manager.Shutdown(&Context);
	EXPECT_TRUE(State->Cancelled.contains(ModeTicket));

	auto ViewportState = std::make_shared<FFakePickingState>();
	uint64 ViewportTicket = 0;
	{
		Durin::Editor::Level::FLevelEditorViewportClient Client;
		Client.InitializeForLevel(Fixture.Level);
		Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(ViewportState));
		const Durin::Editor::Level::FViewportPickSubmission Pick = Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
		ViewportTicket = Pick.Ticket.Id;
	}
	EXPECT_TRUE(ViewportState->Cancelled.contains(ViewportTicket));
}

TEST(FViewportPickingContractTests, SelectModeAppliesCapturedCtrlIntentAfterDeferredCompletion)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	Durin::ACameraActor* Existing = Fixture.Level->SpawnActor<Durin::ACameraActor>("Existing");
	Durin::FTransform ExistingTransform;
	ExistingTransform.Translation = {1000.0, 1000.0, 1000.0};
	Existing->SetActorTransform(ExistingTransform);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Context.SelectActor(Existing);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.bCtrl = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	ASSERT_EQ(State->Requests.size(), 1u);
	const Durin::Editor::Level::FViewportPickTicket Ticket{State->Requests.begin()->first};
	State->CompleteFirst(Ticket);
	Input = {};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	EXPECT_TRUE(Context.IsActorSelected(Existing));
	EXPECT_TRUE(Context.IsActorSelected(Fixture.Actor));
	Manager.Shutdown(&Context);
}

TEST(FViewportPickingContractTests, KeepsViewportTicketAndCompletionStateIndependent)
{
	FPickingFixture Fixture;
	Durin::Editor::Level::FLevelEditorViewportClient SecondClient;
	SecondClient.InitializeForLevel(Fixture.Level);
	auto SharedIndex = std::make_shared<Durin::Editor::Level::FViewportPickingSceneIndex>();
	SharedIndex->SetLevel(Fixture.Level);
	Fixture.Client.SetPickingSceneIndex(SharedIndex);
	SecondClient.SetPickingSceneIndex(SharedIndex);
	auto FirstState = std::make_shared<FFakePickingState>();
	auto SecondState = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(FirstState));
	SecondClient.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(SecondState));
	const Durin::Editor::Level::FViewportPickSubmission FirstPick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	const Durin::Editor::Level::FViewportPickSubmission SecondPick = SecondClient.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	EXPECT_EQ(FirstPick.Ticket.Id, 1u);
	EXPECT_EQ(SecondPick.Ticket.Id, 1u);
	FirstState->CompleteFirst(FirstPick.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(FirstPick.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_EQ(SecondClient.PollViewportPick(SecondPick.Ticket).Status, Durin::Editor::Level::EViewportPickStatus::Pending);
	EXPECT_EQ(SharedIndex->GetDiagnostics().SnapshotBuilds, 1u);
	EXPECT_EQ(SharedIndex->GetDiagnostics().CandidatePrimitives, 2u);
}

TEST(FViewportPickingContractTests, PicksCurrentPoseWithNonContiguousPaletteAndMixedInfluences)
{
	FSkeletalPickingFixture Fixture;
	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend();
	const Durin::Editor::Level::FViewportPickingBackendCompletion Front = Backend->Submit(Fixture.MakeRequest());
	ASSERT_EQ(Front.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Front.Hit);
	EXPECT_EQ(Front.Hit->Token, 1u);
	EXPECT_NEAR(Front.Hit->Distance, 2.0, 1.0e-6);
	EXPECT_EQ(Front.Diagnostics.ApplicableSkeletalTargets, 1u);
	EXPECT_EQ(Front.Diagnostics.SkinnedVertices, 3u);
	EXPECT_EQ(Front.Diagnostics.TestedTriangles, 1u);

	const Durin::Editor::Level::FViewportPickingBackendCompletion Back = Backend->Submit(
		Fixture.MakeRequest({0.0, 0.0, 2.0}, {0.0, 0.0, -1.0}));
	ASSERT_EQ(Back.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_TRUE(Back.Hit);
}

TEST(FViewportPickingContractTests, PicksOneBoneReferencePoseTriangle)
{
	FSkeletalPickingFixture Fixture(false);
	const auto Completion = Durin::Editor::Level::MakeReferenceViewportPickingBackend()->Submit(Fixture.MakeRequest());
	ASSERT_EQ(Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Completion.Hit);
	EXPECT_NEAR(Completion.Hit->Distance, 2.0, 1.0e-6);
	EXPECT_EQ(Completion.Diagnostics.SkinnedVertices, 3u);
	EXPECT_EQ(Completion.Diagnostics.TestedTriangles, 1u);
}

TEST(FViewportPickingContractTests, RejectsPoseBoundsBeforeWorkAndFailsAtomicallyOverBudget)
{
	FSkeletalPickingFixture Fixture;
	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend({3, 1});
	const Durin::Editor::Level::FViewportPickingBackendCompletion AtLimit = Backend->Submit(Fixture.MakeRequest());
	ASSERT_EQ(AtLimit.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_TRUE(AtLimit.Hit);
	EXPECT_EQ(AtLimit.Diagnostics.SkinnedVertices, 3u);
	EXPECT_EQ(AtLimit.Diagnostics.TestedTriangles, 1u);

	Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend({2, 1});
	const Durin::Editor::Level::FViewportPickingBackendCompletion OverLimit = Backend->Submit(Fixture.MakeRequest());
	EXPECT_EQ(OverLimit.Status, Durin::Editor::Level::EViewportPickStatus::Failed);
	EXPECT_FALSE(OverLimit.Hit);
	EXPECT_EQ(OverLimit.Diagnostics.SkeletalBudgetFailures, 1u);
	EXPECT_EQ(OverLimit.Diagnostics.SkinnedVertices, 0u);
	EXPECT_EQ(OverLimit.Diagnostics.TestedTriangles, 0u);

	Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend({0, 0});
	const Durin::Editor::Level::FViewportPickingBackendCompletion BoundsMiss = Backend->Submit(
		Fixture.MakeRequest({10.0, 10.0, -2.0}));
	EXPECT_EQ(BoundsMiss.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_FALSE(BoundsMiss.Hit);
	EXPECT_EQ(BoundsMiss.Diagnostics.SkeletalBoundsRejects, 1u);
	EXPECT_EQ(BoundsMiss.Diagnostics.SkinnedVertices, 0u);
	EXPECT_EQ(BoundsMiss.Diagnostics.TestedTriangles, 0u);
}

TEST(FViewportPickingContractTests, CurrentAnimationPoseAddsAndRemovesSurfaceHits)
{
	FSkeletalPickingFixture Fixture;
	auto ClipPayload = std::make_shared<Durin::FAnimationClipPayloadData>();
	ClipPayload->DurationSeconds = 1.0f;
	ClipPayload->Tracks.push_back({.BoneIndex = 3,
		.Path = Durin::EAnimationTrackPath::Translation,
		.Interpolation = Durin::EAnimationInterpolation::Linear,
		.Times = {0.0f, 1.0f}, .VectorValues = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}}});
	auto* Clip = Durin::NewObject<Durin::DAnimationClip>(Fixture.Level, "SkeletalPickingClip");
	std::string Error;
	ASSERT_TRUE(Clip->PublishBuiltProduct({.Skeleton = Fixture.Skeleton,
		.SkeletonCompatibilityIdentity = Fixture.Skeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Move"), .Payload = std::move(ClipPayload)}, Error)) << Error;
	auto* Component = Fixture.Actor->GetSkeletalMeshComponent();
	ASSERT_TRUE(Component->SetAnimationClip(Clip, Error)) << Error;
	Component->SetLooping(false);
	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend();

	ASSERT_TRUE(Component->Seek(0.0f, Error)) << Error;
	EXPECT_TRUE(Backend->Submit(Fixture.MakeRequest()).Hit);
	ASSERT_TRUE(Component->Seek(1.0f, Error)) << Error;
	const Durin::Editor::Level::FViewportPickingBackendCompletion Moved = Backend->Submit(Fixture.MakeRequest());
	EXPECT_FALSE(Moved.Hit);
	EXPECT_EQ(Moved.Diagnostics.SkinnedVertices, 3u);
	EXPECT_EQ(Moved.Diagnostics.TestedTriangles, 1u);
	ASSERT_TRUE(Component->Seek(0.0f, Error)) << Error;
	EXPECT_TRUE(Backend->Submit(Fixture.MakeRequest()).Hit);
}

TEST(FViewportPickingContractTests, SkeletalSurfaceFlowsThroughExactSemanticIdentity)
{
	FSkeletalPickingFixture Fixture;
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Fixture.Level);
	Fixture.Actor->GetSkeletalMeshComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 3.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	const Durin::Editor::Level::FViewportPickSubmission Pick = Client.SubmitViewportPick(
		Fixture.Level, View, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(Pick.Completion.Hit->Component.Get(), Fixture.Actor->GetSkeletalMeshComponent());
	EXPECT_EQ(Pick.Completion.Hit->PrimitiveId,
		Fixture.Actor->GetSkeletalMeshComponent()->GetPrimitiveSceneId());
}

TEST(FViewportPickingContractTests, SkeletalBudgetFailureLeavesSelectionUnchanged)
{
	FSkeletalPickingFixture Fixture;
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Fixture.Level);
	Client.SetPickingBackendForTesting(Durin::Editor::Level::MakeReferenceViewportPickingBackend({0, 0}));
	Fixture.Actor->GetSkeletalMeshComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 3.0);
	auto* Existing = Fixture.Level->SpawnActor<Durin::ACameraActor>("ExistingSelection");
	ASSERT_NE(Existing, nullptr);
	Existing->GetRootComponent()->SetWorldLocation({1000.0, 1000.0, 1000.0});
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Context.SelectActor(Existing);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Client, View, Input, nullptr));
	EXPECT_TRUE(Context.IsActorSelected(Existing));
	EXPECT_FALSE(Context.IsActorSelected(Fixture.Actor));
	Manager.Shutdown(&Context);
}

TEST(FViewportPickingContractTests, OrdersStaticAndSkeletalByWorldDistanceAndStableKey)
{
	FSkeletalPickingFixture Fixture;
	auto* StaticActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("StaticCompetitor");
	ASSERT_NE(StaticActor, nullptr);
	StaticActor->GetStaticMeshComponent()->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle(Fixture.Level));
	auto* Skeletal = Fixture.Actor->GetSkeletalMeshComponent();
	auto* Static = StaticActor->GetStaticMeshComponent();
	Skeletal->SetWorldLocation({0.0, 0.0, 4.0});
	Static->SetWorldLocation({0.0, 0.0, 2.0});

	auto MakeMixedRequest = [&](bool bReverse, uint64 SkeletalKey, uint64 StaticKey)
	{
		Durin::Editor::Level::FViewportPickingBackendRequest Request{{1}, {0.1, 0.1, 0.0}, {0.0, 0.0, 1.0}};
		Durin::Editor::Level::FViewportPickingTarget SkeletalTarget{1, Skeletal->GetPrimitiveSceneId(), Fixture.Actor,
			Skeletal, SkeletalKey, Skeletal->GetRegistrationGeneration()};
		Durin::Editor::Level::FViewportPickingTarget StaticTarget{2, Static->GetPrimitiveSceneId(), StaticActor,
			Static, StaticKey, Static->GetRegistrationGeneration()};
		Request.Targets = bReverse
			? std::vector<Durin::Editor::Level::FViewportPickingTarget>{StaticTarget, SkeletalTarget}
			: std::vector<Durin::Editor::Level::FViewportPickingTarget>{SkeletalTarget, StaticTarget};
		return Request;
	};

	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend();
	for (bool bReverse : {false, true})
	{
		const auto NearStatic = Backend->Submit(MakeMixedRequest(bReverse, 20, 10));
		ASSERT_TRUE(NearStatic.Hit);
		EXPECT_EQ(NearStatic.Hit->Token, 2u);
	}
	Static->SetWorldLocation({0.0, 0.0, 4.0});
	for (bool bReverse : {false, true})
	{
		const auto StableSkeletal = Backend->Submit(MakeMixedRequest(bReverse, 5, 10));
		ASSERT_TRUE(StableSkeletal.Hit);
		EXPECT_EQ(StableSkeletal.Hit->Token, 1u);
	}
}

TEST(FViewportPickingContractTests, PreservesWorldDistanceUnderMirroringAndRejectsSingularTransforms)
{
	FSkeletalPickingFixture Fixture;
	auto* Component = Fixture.Actor->GetSkeletalMeshComponent();
	Component->SetWorldLocation({0.0, 0.0, 3.0});
	Component->SetWorldScale3D({-2.0, 0.5, 1.0});
	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend();
	const auto Mirrored = Backend->Submit(Fixture.MakeRequest({-0.5, 0.1, 0.0}));
	ASSERT_TRUE(Mirrored.Hit);
	EXPECT_NEAR(Mirrored.Hit->Distance, 3.0, 1.0e-6);
	Component->SetWorldScale3D({2.0, 0.5, 1.5});
	Component->SetWorldRotation(Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		90.0, {0.0, 0.0, 1.0}));
	const auto Rotated = Backend->Submit(Fixture.MakeRequest({0.0, 0.0, 0.0}));
	ASSERT_TRUE(Rotated.Hit);
	EXPECT_NEAR(Rotated.Hit->Distance, 3.0, 1.0e-6);

	Component->SetWorldScale3D({0.0, 1.0, 1.0});
	const auto Singular = Backend->Submit(Fixture.MakeRequest({0.0, 0.1, 0.0}));
	EXPECT_EQ(Singular.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_FALSE(Singular.Hit);
	EXPECT_EQ(Singular.Diagnostics.InvalidSkeletalTargets, 1u);
}

TEST(FViewportPickingContractTests, IntersectsExactSplineMeshDerivedLOD0Surface)
{
	FPickingFixture Fixture;
	auto* Component = Durin::Cast<Durin::DSplineMeshComponent>(Fixture.Actor->AddInstanceComponent(
		Durin::DSplineMeshComponent::StaticClass(), Durin::FName("SplineMeshPicking")));
	ASSERT_NE(Component, nullptr);
	Component->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle(Fixture.Level));
	auto Params = Component->GetSplineMeshParams();
	Params.StartTangent = {100.0, 0.0, 0.0};
	Params.EndPosition = {100.0, 0.0, 0.0};
	Params.EndTangent = {100.0, 0.0, 0.0};
	ASSERT_TRUE(Component->SetSplineMeshParams(Params));
	Component->SetWorldLocation({0.0, 0.0, 3.0});
	ASSERT_TRUE(Component->IsRegistered());

	Durin::Editor::Level::FViewportPickingBackendRequest Request{{1}, {50.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
	Request.Targets.push_back({1, Component->GetPrimitiveSceneId(), Fixture.Actor, Component, 1,
		Component->GetRegistrationGeneration()});
	const auto Completion = Durin::Editor::Level::MakeReferenceViewportPickingBackend()->Submit(std::move(Request));
	ASSERT_EQ(Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Completion.Hit);
	EXPECT_EQ(Completion.Hit->Token, 1u);
	EXPECT_NEAR(Completion.Hit->Distance, 3.0, 1.e-6);
	EXPECT_EQ(Completion.Diagnostics.ApplicableSplineMeshTargets, 1u);
	EXPECT_EQ(Completion.Diagnostics.SplineMeshTestedTriangles, 1u);
}

TEST(FViewportPickingContractTests, SkipsIncompleteIncompatibleAndMalformedSkeletalCandidatesAtomically)
{
	FSkeletalPickingFixture Fixture;
	auto Backend = Durin::Editor::Level::MakeReferenceViewportPickingBackend();
	auto ExpectInvalid = [&]
	{
		const auto Completion = Backend->Submit(Fixture.MakeRequest());
		EXPECT_EQ(Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
		EXPECT_FALSE(Completion.Hit);
		EXPECT_EQ(Completion.Diagnostics.InvalidSkeletalTargets, 1u);
	};

	auto Pose = std::const_pointer_cast<Durin::FSkeletalPosePalette>(
		Fixture.Actor->GetSkeletalMeshComponent()->GetLatestPosePalette());
	ASSERT_TRUE(Pose);
	const std::string Compatibility = Pose->SkeletonCompatibilityIdentity;
	Pose->SkeletonCompatibilityIdentity = "incompatible";
	ExpectInvalid();
	Pose->SkeletonCompatibilityIdentity = Compatibility;

	const std::vector<Durin::FMatrix4f> Matrices = Pose->Matrices;
	Pose->Matrices.pop_back();
	ExpectInvalid();
	Pose->Matrices = Matrices;
	Pose->Matrices[0][0][0] = std::numeric_limits<float>::infinity();
	ExpectInvalid();
	Pose->Matrices = Matrices;

	const auto* RenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	auto& Indices = const_cast<std::vector<uint32>&>(RenderData->IndexBuffer.GetIndices());
	const uint32 FirstIndex = Indices[0];
	Indices[0] = 999;
	ExpectInvalid();
	Indices[0] = FirstIndex;

	auto& Influences = const_cast<std::vector<Durin::FSkeletalMeshVertexInfluences>&>(
		RenderData->VertexBuffers.InfluenceVertexBuffer.GetInfluences());
	const float FirstWeight = Influences[0].Weights[0];
	Influences[0].Weights[0] = -1.0f;
	ExpectInvalid();
	Influences[0].Weights[0] = FirstWeight;

	auto* StaticActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("ValidStaticFallback");
	ASSERT_NE(StaticActor, nullptr);
	StaticActor->GetStaticMeshComponent()->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle(Fixture.Level));
	auto* Static = StaticActor->GetStaticMeshComponent();
	Static->SetWorldLocation({0.0, 0.0, 1.0});
	Pose->SkeletonCompatibilityIdentity = "incompatible";
	auto Request = Fixture.MakeRequest({0.0, 0.0, -2.0});
	Request.Targets.push_back({2, Static->GetPrimitiveSceneId(), StaticActor, Static, 2,
		Static->GetRegistrationGeneration()});
	const auto StaticWinner = Backend->Submit(std::move(Request));
	ASSERT_EQ(StaticWinner.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(StaticWinner.Hit);
	EXPECT_EQ(StaticWinner.Hit->Token, 2u);
	Pose->SkeletonCompatibilityIdentity = Compatibility;
}

TEST(FViewportPickingContractTests, ResolvesExactIdentityAcrossMultipleSkeletalComponents)
{
	FSkeletalPickingFixture Fixture;
	std::string Error;
	auto* Added = Durin::Cast<Durin::DSkeletalMeshComponent>(Fixture.Actor->AddInstanceComponent(
		Durin::DSkeletalMeshComponent::StaticClass(), Durin::FName("AddedSkeletal")));
	ASSERT_NE(Added, nullptr);
	ASSERT_TRUE(Added->SetSkeletalMesh(Fixture.Mesh, Error)) << Error;
	auto* OtherActor = Fixture.Level->SpawnActor<Durin::ASkeletalMeshActor>("OtherSkeletalActor");
	ASSERT_NE(OtherActor, nullptr);
	ASSERT_TRUE(OtherActor->GetSkeletalMeshComponent()->SetSkeletalMesh(Fixture.Mesh, Error)) << Error;

	auto* Original = Fixture.Actor->GetSkeletalMeshComponent();
	Original->SetWorldLocation({0.0, 0.0, 4.0});
	Added->SetWorldLocation({0.0, 0.0, 2.0});
	OtherActor->GetSkeletalMeshComponent()->SetWorldLocation({0.0, 0.0, 3.0});
	Durin::Editor::Level::FViewportPickingBackendRequest Request{{1}, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
	Request.Targets = {
		{1, Original->GetPrimitiveSceneId(), Fixture.Actor, Original, 10, Original->GetRegistrationGeneration()},
		{2, OtherActor->GetSkeletalMeshComponent()->GetPrimitiveSceneId(), OtherActor,
			OtherActor->GetSkeletalMeshComponent(), 20,
			OtherActor->GetSkeletalMeshComponent()->GetRegistrationGeneration()},
		{3, Added->GetPrimitiveSceneId(), Fixture.Actor, Added, 30, Added->GetRegistrationGeneration()}};
	const auto Completion = Durin::Editor::Level::MakeReferenceViewportPickingBackend()->Submit(std::move(Request));
	ASSERT_EQ(Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Completion.Hit);
	EXPECT_EQ(Completion.Hit->Token, 3u);
	EXPECT_EQ(Completion.Diagnostics.ApplicableSkeletalTargets, 3u);
}

TEST(FViewportPickingContractTests, SceneIndexTracksOrderedPrimitiveMutationsAndReducesSparseCandidates)
{
	FPickingFixture Fixture;
	auto Index = std::make_shared<Durin::Editor::Level::FViewportPickingSceneIndex>();
	Index->SetLevel(Fixture.Level);
	Fixture.Client.SetPickingSceneIndex(Index);
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	auto* SharedMesh = Fixture.Actor->GetStaticMeshComponent()->GetStaticMesh();
	for (uint32 ActorIndex = 0; ActorIndex < 100; ++ActorIndex)
	{
		auto* Actor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>(
			Durin::FName(std::format("Sparse{}", ActorIndex)));
		ASSERT_NE(Actor, nullptr);
		Actor->GetStaticMeshComponent()->SetStaticMesh(SharedMesh);
		Actor->GetStaticMeshComponent()->SetWorldLocation({100.0 + ActorIndex * 2.0, 100.0, 3.0});
	}

	const auto Submit = [&]
	{
		const auto Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
		return State->Requests.at(Pick.Ticket.Id).Targets.size();
	};
	EXPECT_EQ(Submit(), 1u);
	const Durin::FVector3 Original = Fixture.Actor->GetStaticMeshComponent()->GetWorldLocation();
	Fixture.Actor->GetStaticMeshComponent()->SetWorldLocation(Original + Durin::FVector3(1000.0, 0.0, 0.0));
	EXPECT_EQ(Submit(), 0u);
	Fixture.Actor->GetStaticMeshComponent()->SetWorldLocation(Original);
	EXPECT_EQ(Submit(), 1u);
	Fixture.Actor->SetHidden(true);
	EXPECT_EQ(Submit(), 0u);
	Fixture.Actor->SetHidden(false);
	EXPECT_EQ(Submit(), 1u);
	Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
	EXPECT_EQ(Submit(), 0u);
	Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	EXPECT_EQ(Submit(), 1u);
	EXPECT_GT(Index->GetDiagnostics().Mutations, 0u);
	EXPECT_GT(Index->GetDiagnostics().Rebuilds, 0u);
	EXPECT_LE(Index->GetDiagnostics().CandidatePrimitives, 5u);
}

TEST(FViewportPickingContractTests, SceneIndexObserverDoesNotOutliveIndexWhenLevelIsPendingKill)
{
	FPickingFixture Fixture;
	auto Index = std::make_shared<Durin::Editor::Level::FViewportPickingSceneIndex>();
	Index->SetLevel(Fixture.Level);

	Durin::MarkObjectHierarchyAsGarbage(Fixture.Level);
	Index.reset();

	Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
	SUCCEED();
}

TEST(FViewportPickingContractTests, StaticAccelerationReusesAssetDataAndMatchesReferenceAndComparePolicies)
{
	FPickingFixture Fixture;
	Fixture.Actor->GetStaticMeshComponent()->SetWorldLocation({0.0, 0.0, 3.0});
	const auto* Data = Fixture.Actor->GetStaticMeshComponent()->GetStaticMesh()->GetRenderData();
	ASSERT_NE(Data, nullptr);
	ASSERT_FALSE(Data->LODResources.empty());
	ASSERT_TRUE(Data->LODResources[0].RayQueryAcceleration);
	auto* Other = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("SharedAcceleration");
	ASSERT_NE(Other, nullptr);
	Other->GetStaticMeshComponent()->SetStaticMesh(Fixture.Actor->GetStaticMeshComponent()->GetStaticMesh());
	EXPECT_EQ(Other->GetStaticMeshComponent()->GetStaticMesh()->GetRenderData()->LODResources[0].RayQueryAcceleration,
		Data->LODResources[0].RayQueryAcceleration);

	const Durin::Editor::Level::FViewportPickingBackendRequest Request = MakeStaticRequest(Fixture.Actor);
	const auto Reference = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Reference)->Submit(Request);
	const auto Accelerated = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated)->Submit(Request);
	const auto Compared = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(Request);
	ASSERT_TRUE(Reference.Hit);
	ASSERT_TRUE(Accelerated.Hit);
	ASSERT_TRUE(Compared.Hit);
	EXPECT_EQ(Reference.Hit->Token, Accelerated.Hit->Token);
	EXPECT_DOUBLE_EQ(Reference.Hit->Distance, Accelerated.Hit->Distance);
	EXPECT_EQ(Compared.Diagnostics.ParityMismatches, 0u);
	EXPECT_EQ(Reference.Diagnostics.StaticTestedTriangles, 1u);
	EXPECT_EQ(Accelerated.Diagnostics.StaticCandidateTriangles, 1u);
	EXPECT_EQ(Accelerated.Diagnostics.StaticReferenceFallbacks, 0u);
	auto& MutableAcceleration = const_cast<Durin::FStaticMeshLODResources&>(Data->LODResources[0]).RayQueryAcceleration;
	const auto SavedAcceleration = MutableAcceleration;
	MutableAcceleration.reset();
	const auto Fallback = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated)->Submit(Request);
	ASSERT_TRUE(Fallback.Hit);
	EXPECT_EQ(Fallback.Hit->Token, Reference.Hit->Token);
	EXPECT_DOUBLE_EQ(Fallback.Hit->Distance, Reference.Hit->Distance);
	EXPECT_EQ(Fallback.Diagnostics.StaticReferenceFallbacks, 1u);
	MutableAcceleration = SavedAcceleration;
}

TEST(FViewportPickingContractTests, RandomizedStaticCompareIsIndependentOfTargetOrder)
{
	FPickingFixture Fixture;
	Durin::DStaticMesh* Mesh = Fixture.Actor->GetStaticMeshComponent()->GetStaticMesh();
	std::mt19937 Generator(0x5A17C3u);
	std::uniform_real_distribution<double> Position(-8.0, 8.0);
	std::vector<Durin::AStaticMeshActor*> Actors{Fixture.Actor};
	for (uint32 Index = 1; Index < 96; ++Index)
	{
		auto* Actor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>(Durin::FName(std::format("Random{}", Index)));
		ASSERT_NE(Actor, nullptr);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actors.push_back(Actor);
	}
	for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
	{
		Durin::Editor::Level::FViewportPickingBackendRequest Request{{Iteration + 1}, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
		for (size_t Index = 0; Index < Actors.size(); ++Index)
		{
			auto* Component = Actors[Index]->GetStaticMeshComponent();
			Component->SetWorldLocation({Position(Generator), Position(Generator), 1.0 + std::abs(Position(Generator))});
			Component->SetWorldScale3D({Iteration % 3 == 0 ? -1.0 : 1.0, 0.5 + (Index % 4), 1.0});
			Request.Targets.push_back({static_cast<uint32>(Index + 1), Component->GetPrimitiveSceneId(),
				Actors[Index], Component, static_cast<uint64>(Index), Component->GetRegistrationGeneration()});
		}
		if ((Iteration & 1u) != 0) std::ranges::reverse(Request.Targets);
		const auto Completion = Durin::Editor::Level::MakeViewportPickingBackend(
			Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(std::move(Request));
		EXPECT_EQ(Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
		EXPECT_EQ(Completion.Diagnostics.ParityMismatches, 0u);
	}
}

TEST(FViewportPickingContractTests, TerrainSurfaceMatchesReferenceAcrossMirroredTransform)
{
	FPickingFixture Fixture;
	auto* Actor = Fixture.Level->SpawnActor<Durin::ATerrainActor>("TerrainPicking");
	ASSERT_NE(Actor, nullptr);
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(Fixture.Level, "PickingHeightmap");
	const std::array<uint16, 6> Samples{0, 0, 0, 0, 65'535, 0};
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(3, 2, Samples, Error)) << Error;
	auto* Component = Actor->GetTerrainComponent();
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(Component->SetHeightRange(2.0, -1.0));
	Component->SetWorldLocation({0.0, 0.0, 4.0});
	Component->SetWorldScale3D({-2.0, 0.5, 1.5});
	Durin::Editor::Level::FViewportPickingBackendRequest Request{
		{1}, {-0.5, 0.25, 10.0}, {0.0, 0.0, -1.0}};
	Request.Targets.push_back({1, Component->GetPrimitiveSceneId(), Actor, Component, 1,
		Component->GetRegistrationGeneration()});
	const auto Reference = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Reference)->Submit(Request);
	const auto Accelerated = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated)->Submit(Request);
	const auto Compare = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Compare)->Submit(Request);
	ASSERT_TRUE(Reference.Hit);
	ASSERT_TRUE(Accelerated.Hit);
	ASSERT_TRUE(Compare.Hit);
	EXPECT_NEAR(Reference.Hit->Distance, Accelerated.Hit->Distance, 1.e-8);
	EXPECT_EQ(Compare.Diagnostics.TerrainParityMismatches, 0u);
	EXPECT_EQ(Accelerated.Diagnostics.ApplicableTerrainTargets, 1u);
}

TEST(FViewportPickingContractTests, TerrainAccelerationBoundsSparseMaximumFixture)
{
	FPickingFixture Fixture;
	auto* Actor = Fixture.Level->SpawnActor<Durin::ATerrainActor>("MaximumTerrain");
	ASSERT_NE(Actor, nullptr);
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(Fixture.Level, "MaximumHeightmap");
	std::vector<uint16> Samples(1025u * 1025u, 0);
	Samples[512u * 1025u + 512u] = 65'535;
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(1025, 1025, Samples, Error)) << Error;
	auto* Component = Actor->GetTerrainComponent();
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(Component->SetHeightRange(1.0, 0.0));
	Durin::Editor::Level::FViewportPickingBackendRequest Request{
		{1}, {10.5, 10.5, 4.0}, {0.0, 0.0, -1.0}};
	Request.Targets.push_back({1, Component->GetPrimitiveSceneId(), Actor, Component, 1,
		Component->GetRegistrationGeneration()});
	const auto Completion = Durin::Editor::Level::MakeViewportPickingBackend(
		Durin::Editor::Level::EViewportPickingBackendPolicy::Accelerated)->Submit(std::move(Request));
	ASSERT_TRUE(Completion.Hit);
	EXPECT_LE(Completion.Diagnostics.TerrainVisitedCells, (1024u * 1024u) / 100u);
	EXPECT_EQ(Completion.Diagnostics.TerrainParityMismatches, 0u);
}
