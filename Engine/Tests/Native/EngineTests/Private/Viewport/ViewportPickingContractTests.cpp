#include "ViewportTestSupport.h"
#include "Components/SplineMeshComponent.h"
#include "LevelEditorViewportEditing.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"
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
		Durin::FStaticMeshDecodedGeometry Imported;
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
		if (!Durin::BuildStaticMeshSynchronously(
			*Result, std::move(Imported), Error)) throw std::runtime_error(Error);
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
