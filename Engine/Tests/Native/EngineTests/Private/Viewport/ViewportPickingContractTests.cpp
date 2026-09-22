#include "ViewportTestSupport.h"
#include "Components/SplineMeshComponent.h"
#include "LevelEditorViewportEditing.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshResources.h"
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
			EXPECT_TRUE(World->InitializeSubsystems());
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
TEST(FViewportPickingContractTests, RejectsOutsideViewAndPreservesExactPrimitiveIdentity)
{
	FPickingFixture Fixture;
	const Durin::Editor::Level::FViewportPickSubmission Outside = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {-1.0f, 300.0f});
	EXPECT_TRUE(Outside.Ticket);
	EXPECT_EQ(Outside.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Invalid);

	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FControlledPickingBackend>(State));
	auto Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	State->CompleteFirst(Pick.Ticket);
	Pick.Completion = Fixture.Client.PollViewportPick(Pick.Ticket);
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Kind, Durin::Editor::Level::EViewportPickHitKind::SceneGeometry);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(Pick.Completion.Hit->Component.Get(), Fixture.Actor->GetStaticMeshComponent());
	EXPECT_EQ(Pick.Completion.Hit->PrimitiveId, Fixture.Actor->GetStaticMeshComponent()->GetPrimitiveComponentId());
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
}


TEST(FViewportPickingContractTests, MissingRendererFailsWithoutCpuFallback)
{
	FPickingFixture Fixture;
	const auto Pick = Fixture.Client.SubmitViewportPick(Fixture.Level, Fixture.View, {400.f, 300.f});
	EXPECT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Failed);
}

TEST(FViewportPickingContractTests, ResolvesOverlayElementAndRejectsRetiredHandle)
{
	using namespace Durin;
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	FViewportPickingService Service(std::make_unique<FControlledPickingBackend>(State));
	Service.SetLevel(Fixture.Level);
	FEditorVisualizationBox Box;
	Box.Actor = Fixture.Actor;
	Box.Component = Fixture.Actor->GetStaticMeshComponent();
	Box.WorldPosition = Fixture.Actor->GetStaticMeshComponent()->GetWorldLocation();
	Box.Element = {EEditorSubElementKind::Point, FGuid::NewGuid()};
	FEditorVisualizationCollector Collector;
	Collector.AddBox(Box);
	const FViewportPickRequest Request{.Level=Fixture.Level, .View=Fixture.View,
		.ViewportPosition={400.f,300.f}, .Layers=EViewportPickLayer::EditorVisualization};
	auto Pick = Service.Submit(Request, &Collector);
	ASSERT_EQ(Pick.Completion.Status, EViewportPickStatus::Pending);
	ASSERT_EQ(State->Requests.at(Pick.Ticket.Id).Overlays.size(), 1u);
	const auto& Overlay = State->Requests.at(Pick.Ticket.Id).Overlays.front();
	EXPECT_TRUE(Overlay.bForeground);
	EXPECT_NEAR((Overlay.Vertices[1].ClipPosition.x - Overlay.Vertices[0].ClipPosition.x) * 400.f,
		Box.SizePixels + 2.f*Box.HitPaddingPixels, 0.01f);
	State->CompleteFirst(Pick.Ticket);
	auto Completion = Service.Poll(Pick.Ticket);
	ASSERT_TRUE(Completion.Hit);
	EXPECT_EQ(Completion.Hit->Element, Box.Element);
	EXPECT_EQ(Completion.Hit->Component.Get(), Box.Component.Get());
	Pick = Service.Submit(Request, &Collector);
	Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
	Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	State->CompleteFirst(Pick.Ticket);
	EXPECT_EQ(Service.Poll(Pick.Ticket).Status, EViewportPickStatus::Invalidated);
}
