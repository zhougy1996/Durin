#include "ViewportTestSupport.h"
#include "LevelEditorViewportEditing.h"
#include "Viewport/ViewportPickingService.h"

namespace
{
	struct FFakePickingState
	{
		std::unordered_map<Durin::uint64, Durin::FViewportPickingBackendRequest> Requests;
		std::unordered_map<Durin::uint64, Durin::FViewportPickingBackendCompletion> Completions;
		std::unordered_set<Durin::uint64> Cancelled;

		auto CompleteFirst(Durin::FViewportPickTicket Ticket, double Distance = 3.0) -> void
		{
			const auto It = Requests.find(Ticket.Id);
			ASSERT_NE(It, Requests.end());
			ASSERT_FALSE(It->second.Targets.empty());
			Completions[Ticket.Id] = {
				Durin::EViewportPickStatus::Completed,
				Durin::FViewportPickingBackendHit{It->second.Targets.front().Token, Distance, 0},
			};
		}
	};

	class FControlledPickingBackend final : public Durin::IViewportPickingBackend
	{
	public:
		explicit FControlledPickingBackend(std::shared_ptr<FFakePickingState> InState) : State(std::move(InState)) {}

		auto Submit(Durin::FViewportPickingBackendRequest Request) -> Durin::FViewportPickingBackendCompletion override
		{
			State->Requests.emplace(Request.Ticket.Id, std::move(Request));
			return {Durin::EViewportPickStatus::Pending, std::nullopt};
		}

		auto Poll(Durin::FViewportPickTicket Ticket) -> Durin::FViewportPickingBackendCompletion override
		{
			const auto It = State->Completions.find(Ticket.Id);
			return It == State->Completions.end()
				? Durin::FViewportPickingBackendCompletion{Durin::EViewportPickStatus::Pending, std::nullopt}
				: It->second;
		}

		auto Cancel(Durin::FViewportPickTicket Ticket) -> void override { State->Cancelled.insert(Ticket.Id); }

	private:
		std::shared_ptr<FFakePickingState> State;
	};

	struct FPickingFixture
	{
		Durin::DWorld* World = nullptr;
		Durin::DLevel* Level = nullptr;
		Durin::AStaticMeshActor* Actor = nullptr;
		Durin::FLevelEditorViewportClient Client;
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
}

TEST(FViewportPickingContractTests, AppliesFrozenCrossFamilyOrdering)
{
	Durin::FViewportPickHit Geometry{.Kind = Durin::EViewportPickHitKind::SceneGeometry, .Distance = 5.0, .Priority = 0, .StableTieKey = 20};
	Durin::FViewportPickHit Visualization{.Kind = Durin::EViewportPickHitKind::EditorVisualization, .Distance = 4.0, .Priority = 100, .StableTieKey = 10};
	EXPECT_TRUE(Durin::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.Distance = 6.0;
	EXPECT_FALSE(Durin::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.bDepthIndependent = true;
	EXPECT_TRUE(Durin::IsViewportPickHitPreferred(Visualization, Geometry));
	Visualization.bDepthIndependent = false;
	Visualization.Distance = Geometry.Distance;
	EXPECT_FALSE(Durin::IsViewportPickHitPreferred(Visualization, Geometry));
	Durin::FViewportPickHit Stable = Geometry;
	Stable.StableTieKey = 5;
	EXPECT_TRUE(Durin::IsViewportPickHitPreferred(Stable, Geometry));
}

TEST(FViewportPickingContractTests, RejectsOutsideViewAndPreservesExactPrimitiveIdentity)
{
	FPickingFixture Fixture;
	const Durin::FViewportPickSubmission Outside = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {-1.0f, 300.0f});
	EXPECT_TRUE(Outside.Ticket);
	EXPECT_EQ(Outside.Completion.Status, Durin::EViewportPickStatus::Invalid);

	const Durin::FViewportPickSubmission Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Kind, Durin::EViewportPickHitKind::SceneGeometry);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(Pick.Completion.Hit->Component.Get(), Fixture.Actor->GetStaticMeshComponent());
	EXPECT_EQ(Pick.Completion.Hit->PrimitiveId, Fixture.Actor->GetStaticMeshComponent()->GetPrimitiveSceneId());
}

TEST(FViewportPickingContractTests, SupportsPendingPollingCancellationAndSupersession)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	const Durin::FViewportPickSubmission First = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	ASSERT_EQ(First.Completion.Status, Durin::EViewportPickStatus::Pending);
	EXPECT_EQ(Fixture.Client.PollViewportPick(First.Ticket).Status, Durin::EViewportPickStatus::Pending);

	const Durin::FViewportPickSubmission Second = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	EXPECT_EQ(Fixture.Client.PollViewportPick(First.Ticket).Status, Durin::EViewportPickStatus::Cancelled);
	EXPECT_TRUE(State->Cancelled.contains(First.Ticket.Id));
	State->CompleteFirst(Second.Ticket);
	const Durin::FViewportPickCompletion Completed = Fixture.Client.PollViewportPick(Second.Ticket);
	ASSERT_EQ(Completed.Status, Durin::EViewportPickStatus::Completed);
	ASSERT_TRUE(Completed.Hit);
	EXPECT_EQ(Completed.Hit->Actor.Get(), Fixture.Actor);

	const Durin::FViewportPickSubmission Third = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.CancelViewportPick(Third.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Third.Ticket).Status, Durin::EViewportPickStatus::Cancelled);
}

TEST(FViewportPickingContractTests, InvalidatesRetiredTargetsButNotCameraMotion)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	const Durin::FViewportPickSubmission CameraStable = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.FocusLocation({100.0, 200.0, 300.0});
	State->CompleteFirst(CameraStable.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(CameraStable.Ticket).Status, Durin::EViewportPickStatus::Completed);

	const Durin::FViewportPickSubmission Hidden = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	State->CompleteFirst(Hidden.Ticket);
	Fixture.Actor->SetHidden(true);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Hidden.Ticket).Status, Durin::EViewportPickStatus::Invalidated);
	Fixture.Actor->SetHidden(false);

	const Durin::FViewportPickSubmission Reregistered = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
	Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	State->CompleteFirst(Reregistered.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Reregistered.Ticket).Status, Durin::EViewportPickStatus::Invalidated);

	const Durin::FViewportPickSubmission Reset = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	Fixture.Client.InitializeForLevel(Fixture.Level);
	EXPECT_EQ(Fixture.Client.PollViewportPick(Reset.Ticket).Status, Durin::EViewportPickStatus::Invalidated);

	const Durin::FViewportPickSubmission Destroyed = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	State->CompleteFirst(Destroyed.Ticket);
	ASSERT_TRUE(Fixture.Level->DestroyActor(Fixture.Actor));
	Durin::CollectGarbage();
	EXPECT_EQ(Fixture.Client.PollViewportPick(Destroyed.Ticket).Status, Durin::EViewportPickStatus::Invalidated);
}

TEST(FViewportPickingContractTests, CancelsPendingWorkOnModeAndViewportExit)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	Durin::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Durin::FLevelViewportEditModeManager Manager;
	Durin::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	ASSERT_EQ(State->Requests.size(), 1u);
	const Durin::uint64 ModeTicket = State->Requests.begin()->first;
	Manager.Shutdown(&Context);
	EXPECT_TRUE(State->Cancelled.contains(ModeTicket));

	auto ViewportState = std::make_shared<FFakePickingState>();
	Durin::uint64 ViewportTicket = 0;
	{
		Durin::FLevelEditorViewportClient Client;
		Client.InitializeForLevel(Fixture.Level);
		Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(ViewportState));
		const Durin::FViewportPickSubmission Pick = Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
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
	Durin::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Context.SelectActor(Existing);
	Durin::FLevelViewportEditModeManager Manager;
	Durin::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.bCtrl = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	ASSERT_EQ(State->Requests.size(), 1u);
	const Durin::FViewportPickTicket Ticket{State->Requests.begin()->first};
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
	Durin::FLevelEditorViewportClient SecondClient;
	SecondClient.InitializeForLevel(Fixture.Level);
	auto FirstState = std::make_shared<FFakePickingState>();
	auto SecondState = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(FirstState));
	SecondClient.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(SecondState));
	const Durin::FViewportPickSubmission FirstPick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	const Durin::FViewportPickSubmission SecondPick = SecondClient.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	EXPECT_EQ(FirstPick.Ticket.Id, 1u);
	EXPECT_EQ(SecondPick.Ticket.Id, 1u);
	FirstState->CompleteFirst(FirstPick.Ticket);
	EXPECT_EQ(Fixture.Client.PollViewportPick(FirstPick.Ticket).Status, Durin::EViewportPickStatus::Completed);
	EXPECT_EQ(SecondClient.PollViewportPick(SecondPick.Ticket).Status, Durin::EViewportPickStatus::Pending);
}
