#include "ViewportTestSupport.h"

TEST(FEditorTransactionManagerTests, ExecutesUndoesRedoesAndClearsRedoBranch)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.CanUndo());
	EXPECT_EQ(Manager.GetUndoDescription(), "Counting");
	ASSERT_TRUE(Manager.Undo());
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.CanRedo());
	ASSERT_TRUE(Manager.Redo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Manager.Undo());
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value, 2)));
	EXPECT_EQ(Value, 2);
	EXPECT_FALSE(Manager.CanRedo());
	Manager.Clear();
	EXPECT_FALSE(Manager.CanUndo());
}

TEST(FEditorTransactionManagerTests, UsesStableIdsAndRejectsStaleUndoRequests)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId FirstId = Manager.GetUndoId();
	ASSERT_NE(FirstId, 0);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId SecondId = Manager.GetUndoId();
	ASSERT_NE(SecondId, FirstId);

	EXPECT_FALSE(Manager.Undo(FirstId));
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Manager.Undo(SecondId));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsRedoHead(SecondId));
	ASSERT_TRUE(Manager.Redo(SecondId));
	EXPECT_EQ(Value, 2);
	EXPECT_TRUE(Manager.IsUndoHead(SecondId));

	const std::vector<Durin::FEditorTransactionEvent> Events = Manager.ConsumeEvents();
	ASSERT_EQ(Events.size(), 4);
	EXPECT_EQ(Events[0].Type, Durin::EEditorTransactionEventType::Executed);
	EXPECT_EQ(Events[0].Details, "Counter changed forward");
	EXPECT_EQ(Events[1].Type, Durin::EEditorTransactionEventType::Executed);
	EXPECT_EQ(Events[2].Type, Durin::EEditorTransactionEventType::Undone);
	EXPECT_EQ(Events[2].Details, "Counter changed backward");
	EXPECT_EQ(Events[3].Type, Durin::EEditorTransactionEventType::Redone);
	EXPECT_EQ(Events[3].Details, "Counter changed forward");
}

TEST(FEditorTransactionManagerTests, KeepsTransactionsOnTheirOriginalStackWhenApplyFails)
{
	int Value = 0;
	FTransactionControl Control;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FControlledTransaction>(Value, Control)));
	const Durin::FEditorTransactionId Id = Manager.GetUndoId();
	Manager.ConsumeEvents();

	Control.bFailUndo = true;
	EXPECT_FALSE(Manager.Undo(Id));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsUndoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::EEditorTransactionEventType::Failed);

	Control.bFailUndo = false;
	ASSERT_TRUE(Manager.Undo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	Manager.ConsumeEvents();

	Control.bFailRedo = true;
	EXPECT_FALSE(Manager.Redo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::EEditorTransactionEventType::Failed);
}

TEST(FEditorTransactionManagerTests, ClearsRedoBranchAndPendingEventsOnNewCommitAndClear)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId OldId = Manager.GetUndoId();
	ASSERT_TRUE(Manager.Undo(OldId));
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value, 2)));
	EXPECT_FALSE(Manager.CanRedo());
	EXPECT_FALSE(Manager.Redo(OldId));

	Manager.Clear();
	EXPECT_TRUE(Manager.ConsumeEvents().empty());
	EXPECT_FALSE(Manager.CanUndo());
}

TEST(FViewportCameraTransformTests, ClampsPitchAndBuildsOrthonormalDirections)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Rotate(0.0f, 200.0f);
	EXPECT_DOUBLE_EQ(Camera.GetPitch(), 89.0);
	EXPECT_NEAR(glm::length(Camera.GetForwardVector()), 1.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetRightVector()), 0.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetUpVector()), 0.0, 1.e-8);
}

TEST(FViewportCameraTransformTests, MovesPansAndPreservesOrbitDistance)
{
	Durin::FViewportCameraTransform Camera;
	const Durin::FVector3 InitialLocation = Camera.GetLocation();
	const Durin::FVector3 InitialPivot = Camera.GetOrbitPivot();
	Camera.MoveLocal({2.0, 0.0, 0.0});
	ExpectVectorNear(Camera.GetOrbitPivot() - InitialPivot, Camera.GetLocation() - InitialLocation);

	Camera.Pan(1.0f, -0.5f);
	const double Distance = Camera.GetOrbitDistance();
	Camera.Orbit(35.0f, 15.0f);
	EXPECT_NEAR(glm::length(Camera.GetOrbitPivot() - Camera.GetLocation()), Distance, 1.e-8);
}

TEST(FViewportCameraTransformTests, FocusAndDollyRemainFiniteAtDegenerateDistance)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Focus({3.0, 4.0, 5.0}, 0.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	ExpectVectorNear(Camera.GetOrbitPivot(), {3.0, 4.0, 5.0});
	Camera.Dolly(100000.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	const Durin::FVector3 Location = Camera.GetLocation();
	EXPECT_TRUE(std::isfinite(Location.x) && std::isfinite(Location.y) && std::isfinite(Location.z));
}

TEST(FViewportCameraTransformTests, RestoresCompleteStateAndConstrainsInvalidNavigationValues)
{
	Durin::FViewportCameraTransform Camera;
	Durin::FLevelViewportCameraState State;
	State.Location = {10.0, 20.0, 30.0};
	State.OrbitPivot = {1.0, 2.0, 3.0};
	State.OrbitDistance = -4.0;
	State.Pitch = 200.0;
	State.Yaw = 123.0;
	Camera.SetState(State);
	const Durin::FLevelViewportCameraState Actual = Camera.GetState();
	ExpectVectorNear(Actual.Location, State.Location);
	ExpectVectorNear(Actual.OrbitPivot, State.OrbitPivot);
	EXPECT_DOUBLE_EQ(Actual.OrbitDistance, 0.05);
	EXPECT_DOUBLE_EQ(Actual.Pitch, 89.0);
	EXPECT_DOUBLE_EQ(Actual.Yaw, State.Yaw);
}

TEST(FLevelViewportSessionSettingsTests, RoundTripsProjectsAndLevelsAndSkipsInvalidEntries)
{
	Durin::FLevelViewportStateMap States;
	States["G:/Projects/A/A.dproject"]["/A/Levels/Main"] = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}, 7.0, -8.0, 9.0};
	States["G:/Projects/B/B.dproject"]["/B/Levels/Other"] = {{10.0, 20.0, 30.0}, {40.0, 50.0, 60.0}, 70.0, -80.0, 90.0};
	Durin::FYamlDocument Document;
	Durin::FYamlNodeRef Root = Document.GetMutableRoot();
	Root.EnsureMap();
	Durin::SaveLevelViewportStates(Root, States);
	Durin::FYamlNodeRef Invalid = Root.GetRef("LevelViewportStates").AppendMap();
	Invalid.SetChildValue("Project", "G:/Projects/A/A.dproject");
	Invalid.SetChildValue("Level", "/A/Levels/Broken");
	Invalid.AddSequence("Location").AppendValue(1.0).AppendValue(2.0);

	Durin::FLevelViewportStateMap Loaded;
	Durin::LoadLevelViewportStates(Document.GetRootView(), Loaded);
	ASSERT_EQ(Loaded.size(), 2u);
	ASSERT_EQ(Loaded.at("G:/Projects/A/A.dproject").size(), 1u);
	const Durin::FLevelViewportCameraState& Main = Loaded.at("G:/Projects/A/A.dproject").at("/A/Levels/Main");
	ExpectVectorNear(Main.Location, {1.0, 2.0, 3.0});
	ExpectVectorNear(Main.OrbitPivot, {4.0, 5.0, 6.0});
	EXPECT_DOUBLE_EQ(Main.OrbitDistance, 7.0);
	EXPECT_DOUBLE_EQ(Main.Pitch, -8.0);
	EXPECT_DOUBLE_EQ(Main.Yaw, 9.0);
}
