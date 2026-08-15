#include "ViewportTestSupport.h"
#include "Application/MonaEventHandler.h"
#include "Documents/LevelDocumentRevisionState.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Runtime/MonaImGui/Private/ImGuiMonaImpl.h"
#include "Viewport/ViewportPresentation.h"
#include "Window/GenericWindow.h"

namespace
{
	class FTestCursorWindow final : public Durin::FGenericWindow
	{
	public:
		auto GetCursorPosition() const -> Durin::FVector2d override { return Position; }
		auto SetCursorPosition(Durin::FVector2d InPosition) -> void override
		{
			Position = InPosition;
			Operations.emplace_back("position");
		}
		auto SetPosition(Durin::FVector2d InPosition) -> void { Position = InPosition; }
		auto GetOperations() const -> const std::vector<std::string>& { return Operations; }

	protected:
		auto ApplyCursorMode(Durin::ECursorMode InCursorMode) -> void override
		{
			Operations.emplace_back(InCursorMode == Durin::ECursorMode::Captured ? "captured" : "free");
		}

	private:
		Durin::FVector2d Position{0.0};
		std::vector<std::string> Operations;
	};
}

TEST(FGenericWindowCursorTests, KeepsCursorShapeAndModeAsIndependentState)
{
	FTestCursorWindow Window;
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Free);
	Window.SetPosition({31.0, 47.0});
	Window.SetCursor(Durin::EMouseCursor::Hand);
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Free);
	Window.SetCursorMode(Durin::ECursorMode::Captured);
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Captured);
	Window.SetPosition({900.0, 700.0});
	Window.SetCursor(Durin::EMouseCursor::TextInput);
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Captured);
	Window.SetCursorMode(Durin::ECursorMode::Captured);
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Captured);
	Window.SetCursorMode(Durin::ECursorMode::Free);
	EXPECT_EQ(Window.GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_EQ(Window.GetCursorPosition(), Durin::FVector2d(31.0, 47.0));
	EXPECT_EQ(Window.GetOperations(), (std::vector<std::string>{"captured", "free", "position"}));
}

TEST(FMonaImGuiInputTests, KeepsCapturedVirtualMouseEventsOutOfTheUI)
{
	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(800.0f, 600.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();

	const std::unique_ptr<Durin::Mona::FMonaEventHandler> Handler =
		Durin::MonaImGui::ImGuiMonaImpl_CreateEventHandler();
	const std::shared_ptr<FTestCursorWindow> Window = std::make_shared<FTestCursorWindow>();

	auto DrawFrame = [&]() {
		ImGui::NewFrame();
		ImGui::EndFrame();
	};

	Handler->OnMouseMove(Window, {64.0, 48.0});
	Handler->OnMouseDown(Window, Durin::EMouseButton::Left, {64.0, 48.0});
	DrawFrame();
	EXPECT_FLOAT_EQ(IO.MousePos.x, 64.0f);
	EXPECT_FLOAT_EQ(IO.MousePos.y, 48.0f);
	EXPECT_TRUE(IO.MouseDown[ImGuiMouseButton_Left]);

	Window->SetCursorMode(Durin::ECursorMode::Captured);
	EXPECT_FALSE(Handler->OnMouseMove(Window, {100000.0, -100000.0}));
	EXPECT_FALSE(Handler->OnMouseDown(Window, Durin::EMouseButton::Right, {100000.0, -100000.0}));
	EXPECT_FALSE(Handler->OnMouseWheel(Window, 3.0, 7.0));
	Handler->OnMouseLeave(Window);
	Handler->OnMouseEnter(Window);
	EXPECT_FALSE(Handler->OnMouseUp(Window, Durin::EMouseButton::Left, {100000.0, -100000.0}));
	DrawFrame();

	EXPECT_FLOAT_EQ(IO.MousePos.x, 64.0f);
	EXPECT_FLOAT_EQ(IO.MousePos.y, 48.0f);
	EXPECT_FALSE(IO.MouseDown[ImGuiMouseButton_Left]);
	EXPECT_FALSE(IO.MouseDown[ImGuiMouseButton_Right]);
	EXPECT_FLOAT_EQ(IO.MouseWheel, 0.0f);

	Window->SetCursorMode(Durin::ECursorMode::Free);
	Handler->OnMouseMove(Window, {12.0, 24.0});
	DrawFrame();
	EXPECT_FLOAT_EQ(IO.MousePos.x, 12.0f);
	EXPECT_FLOAT_EQ(IO.MousePos.y, 24.0f);

	ImGui::DestroyContext(Context);
}

TEST(FViewportStatisticsOverlayTests, FormatsBoundedCountsCompactly)
{
	using Durin::Editor::Level::FormatViewportStatistic;
	EXPECT_EQ(FormatViewportStatistic(0), "0");
	EXPECT_EQ(FormatViewportStatistic(999), "999");
	EXPECT_EQ(FormatViewportStatistic(1'250), "1.2K");
	EXPECT_EQ(FormatViewportStatistic(1'250'000), "1.25M");
	EXPECT_EQ(FormatViewportStatistic(2'500'000'000), "2.50B");
}

TEST(FViewportStatisticsOverlayTests, AnchorsBelowBadgeAndSuppressesUnreadablePanel)
{
	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	ImGui::NewFrame();

	const auto Layout =
		Durin::Editor::Level::CalculateViewportStatisticsOverlayLayout(
			ImVec2(100.0f, 50.0f), ImVec2(900.0f, 650.0f), true);
	EXPECT_TRUE(Layout.bPanelVisible);
	EXPECT_FLOAT_EQ(Layout.PanelMax.x, Layout.BadgeMax.x);
	EXPECT_GT(Layout.PanelMin.y, Layout.BadgeMax.y);
	EXPECT_TRUE(Layout.Contains(ImVec2(
		(Layout.BadgeMin.x + Layout.BadgeMax.x) * 0.5f,
		(Layout.BadgeMin.y + Layout.BadgeMax.y) * 0.5f)));
	EXPECT_TRUE(Layout.Contains(ImVec2(
		(Layout.PanelMin.x + Layout.PanelMax.x) * 0.5f,
		(Layout.PanelMin.y + Layout.PanelMax.y) * 0.5f)));

	const auto Narrow =
		Durin::Editor::Level::CalculateViewportStatisticsOverlayLayout(
			ImVec2(0.0f, 0.0f), ImVec2(180.0f, 500.0f), true);
	EXPECT_FALSE(Narrow.bPanelVisible);
	EXPECT_TRUE(Narrow.Contains(ImVec2(
		(Narrow.BadgeMin.x + Narrow.BadgeMax.x) * 0.5f,
		(Narrow.BadgeMin.y + Narrow.BadgeMax.y) * 0.5f)));

	ImGui::EndFrame();
	ImGui::DestroyContext(Context);
}

TEST(FEditorTransactionManagerTests, ExecutesUndoesRedoesAndClearsRedoBranch)
{
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
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
	Durin::Editor::FTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::Editor::FTransactionId FirstId = Manager.GetUndoId();
	ASSERT_NE(FirstId, 0);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::Editor::FTransactionId SecondId = Manager.GetUndoId();
	ASSERT_NE(SecondId, FirstId);

	EXPECT_FALSE(Manager.Undo(FirstId));
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Manager.Undo(SecondId));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsRedoHead(SecondId));
	ASSERT_TRUE(Manager.Redo(SecondId));
	EXPECT_EQ(Value, 2);
	EXPECT_TRUE(Manager.IsUndoHead(SecondId));

	const std::vector<Durin::Editor::FTransactionEvent> Events = Manager.ConsumeEvents();
	ASSERT_EQ(Events.size(), 4);
	EXPECT_EQ(Events[0].Type, Durin::Editor::ETransactionEventType::Executed);
	EXPECT_EQ(Events[0].Details, "Counter changed forward");
	EXPECT_EQ(Events[1].Type, Durin::Editor::ETransactionEventType::Executed);
	EXPECT_EQ(Events[2].Type, Durin::Editor::ETransactionEventType::Undone);
	EXPECT_EQ(Events[2].Details, "Counter changed backward");
	EXPECT_EQ(Events[3].Type, Durin::Editor::ETransactionEventType::Redone);
	EXPECT_EQ(Events[3].Details, "Counter changed forward");
}

TEST(FEditorTransactionManagerTests, KeepsTransactionsOnTheirOriginalStackWhenApplyFails)
{
	int Value = 0;
	FTransactionControl Control;
	Durin::Editor::FTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FControlledTransaction>(Value, Control)));
	const Durin::Editor::FTransactionId Id = Manager.GetUndoId();
	Manager.ConsumeEvents();

	Control.bFailUndo = true;
	EXPECT_FALSE(Manager.Undo(Id));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsUndoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::Editor::ETransactionEventType::Failed);

	Control.bFailUndo = false;
	ASSERT_TRUE(Manager.Undo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	Manager.ConsumeEvents();

	Control.bFailRedo = true;
	EXPECT_FALSE(Manager.Redo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::Editor::ETransactionEventType::Failed);
}

TEST(FEditorTransactionManagerTests, ClearsRedoBranchAndPendingEventsOnNewCommitAndClear)
{
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::Editor::FTransactionId OldId = Manager.GetUndoId();
	ASSERT_TRUE(Manager.Undo(OldId));
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value, 2)));
	EXPECT_FALSE(Manager.CanRedo());
	EXPECT_FALSE(Manager.Redo(OldId));

	Manager.Clear();
	EXPECT_TRUE(Manager.ConsumeEvents().empty());
	EXPECT_FALSE(Manager.CanUndo());
}

TEST(FEditorTransactionManagerTests, TracksSavedCheckpointAcrossCommitUndoRedoAndMiddleSave)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Package);
	const Durin::Editor::FPackageRevisionState Initial = *Manager.GetPackageRevisionState(*Package);
	EXPECT_TRUE(Initial.bCheckpointValid);
	EXPECT_EQ(Initial.CurrentRevision, Initial.SavedRevision);
	EXPECT_FALSE(Package->IsDirty());

	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	const Durin::Editor::FPackageRevisionState Edited = *Manager.GetPackageRevisionState(*Package);
	EXPECT_NE(Edited.CurrentRevision, Edited.SavedRevision);
	EXPECT_TRUE(Package->IsDirty());

	ASSERT_TRUE(Manager.Undo());
	EXPECT_EQ(Manager.GetPackageRevisionState(*Package)->CurrentRevision, Initial.SavedRevision);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Manager.Redo());
	EXPECT_EQ(Manager.GetPackageRevisionState(*Package)->CurrentRevision, Edited.CurrentRevision);
	EXPECT_TRUE(Package->IsDirty());

	Manager.MarkSaved(*Package);
	const Durin::Editor::FPackageRevisionState MiddleSave = *Manager.GetPackageRevisionState(*Package);
	EXPECT_EQ(MiddleSave.CurrentRevision, MiddleSave.SavedRevision);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Manager.Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Manager.Redo());
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FEditorTransactionManagerTests, AllocatesFreshRevisionWhenReplacingSavedRedoBranch)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Package);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	Manager.MarkSaved(*Package);
	const Durin::Editor::FRevisionId DiscardedSavedRevision = Manager.GetPackageRevisionState(*Package)->SavedRevision;
	ASSERT_TRUE(Manager.Undo());

	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package}, 2)));
	const Durin::Editor::FPackageRevisionState Branched = *Manager.GetPackageRevisionState(*Package);
	EXPECT_NE(Branched.CurrentRevision, DiscardedSavedRevision);
	EXPECT_EQ(Branched.SavedRevision, DiscardedSavedRevision);
	EXPECT_TRUE(Branched.bCheckpointValid);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Manager.CanRedo());
}

TEST(FEditorTransactionManagerTests, AppliesMultiPackageTransitionsAndPreservesStateOnFailure)
{
	Durin::DPackage* First = MakeRevisionTestPackage("First");
	Durin::DPackage* Second = MakeRevisionTestPackage("Second");
	int Value = 0;
	FTransactionControl Control;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*First);
	Manager.EstablishSavedState(*Second);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(
		Value, std::initializer_list{First, Second, First}, 1, &Control
	)));
	EXPECT_TRUE(First->IsDirty());
	EXPECT_TRUE(Second->IsDirty());
	const Durin::Editor::FPackageRevisionState FirstEdited = *Manager.GetPackageRevisionState(*First);
	const Durin::Editor::FPackageRevisionState SecondEdited = *Manager.GetPackageRevisionState(*Second);

	Control.bFailUndo = true;
	EXPECT_FALSE(Manager.Undo());
	EXPECT_EQ(Manager.GetPackageRevisionState(*First)->CurrentRevision, FirstEdited.CurrentRevision);
	EXPECT_EQ(Manager.GetPackageRevisionState(*Second)->CurrentRevision, SecondEdited.CurrentRevision);
	EXPECT_TRUE(First->IsDirty());
	EXPECT_TRUE(Second->IsDirty());

	Control.bFailUndo = false;
	ASSERT_TRUE(Manager.Undo());
	EXPECT_FALSE(First->IsDirty());
	EXPECT_FALSE(Second->IsDirty());
	Control.bFailRedo = true;
	EXPECT_FALSE(Manager.Redo());
	EXPECT_FALSE(First->IsDirty());
	EXPECT_FALSE(Second->IsDirty());
}

TEST(FEditorTransactionManagerTests, FailedExecuteDoesNotCreateRevisionState)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	FTransactionControl Control;
	Control.bFailRedo = true;
	Durin::Editor::FTransactionManager Manager;
	EXPECT_FALSE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(
		Value, std::initializer_list{Package}, 1, &Control
	)));
	EXPECT_EQ(Value, 0);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*Package).has_value());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(Manager.CanUndo());
	EXPECT_FALSE(Manager.CanRedo());
}

TEST(FEditorTransactionManagerTests, InvalidCheckpointStaysDirtyUntilResave)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Package);
	Manager.InvalidateSavedState(*Package);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*Package)->bCheckpointValid);
	EXPECT_TRUE(Package->IsDirty());

	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	ASSERT_TRUE(Manager.Undo());
	EXPECT_TRUE(Package->IsDirty());
	Manager.MarkSaved(*Package);
	EXPECT_TRUE(Manager.GetPackageRevisionState(*Package)->bCheckpointValid);
	EXPECT_FALSE(Package->IsDirty());

	Manager.ForgetPackage(*Package);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*Package).has_value());
	EXPECT_FALSE(Manager.CanRedo());
}

TEST(FEditorTransactionManagerTests, HistoryEvictionDoesNotChangeSavedRevisionComparison)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Package);
	for (int Index = 0; Index < 256; ++Index)
	{
		ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	}
	Manager.MarkSaved(*Package);
	const Durin::Editor::FRevisionId SavedRevision = Manager.GetPackageRevisionState(*Package)->SavedRevision;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Manager.Undo());
	EXPECT_EQ(Manager.GetPackageRevisionState(*Package)->CurrentRevision, SavedRevision);
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FEditorTransactionManagerTests, ClearForgetsMetadataWithoutReusingRevisionIds)
{
	Durin::DPackage* First = MakeRevisionTestPackage("BeforeClear");
	Durin::DPackage* Second = MakeRevisionTestPackage("AfterClear");
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*First);
	const Durin::Editor::FRevisionId BeforeClear = Manager.GetPackageRevisionState(*First)->CurrentRevision;
	Manager.Clear();
	EXPECT_FALSE(Manager.GetPackageRevisionState(*First).has_value());
	Manager.EstablishSavedState(*Second);
	EXPECT_GT(Manager.GetPackageRevisionState(*Second)->CurrentRevision, BeforeClear);
}

TEST(FLevelDocumentRevisionStateTests, AdvancesCheckpointOnlyAfterSuccessfulSave)
{
	Durin::DPackage* Package = MakeRevisionTestPackage();
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Package);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Package})));
	const Durin::Editor::FPackageRevisionState BeforeFailure = *Manager.GetPackageRevisionState(*Package);

	Durin::Editor::Level::FLevelDocumentRevisionState::CompleteSave(&Manager, *Package, false);
	EXPECT_EQ(Manager.GetPackageRevisionState(*Package)->SavedRevision, BeforeFailure.SavedRevision);
	EXPECT_TRUE(Package->IsDirty());
	Durin::Editor::Level::FLevelDocumentRevisionState::CompleteSave(&Manager, *Package, true);
	EXPECT_EQ(
		Manager.GetPackageRevisionState(*Package)->SavedRevision,
		Manager.GetPackageRevisionState(*Package)->CurrentRevision
	);
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FLevelDocumentRevisionStateTests, ReplacesActivationStateAndDiscardsMetadata)
{
	Durin::DPackage* Previous = MakeRevisionTestPackage("Previous");
	Durin::DPackage* CleanReplacement = MakeRevisionTestPackage("CleanReplacement");
	Durin::DPackage* DirtyReplacement = MakeRevisionTestPackage("DirtyReplacement");
	int Value = 0;
	Durin::Editor::FTransactionManager Manager;
	Manager.EstablishSavedState(*Previous);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FPackageCountingTransaction>(Value, std::initializer_list{Previous})));

	Durin::Editor::Level::FLevelDocumentRevisionState::Activate(&Manager, CleanReplacement);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*Previous).has_value());
	ASSERT_TRUE(Manager.GetPackageRevisionState(*CleanReplacement).has_value());
	EXPECT_TRUE(Manager.GetPackageRevisionState(*CleanReplacement)->bCheckpointValid);
	EXPECT_FALSE(CleanReplacement->IsDirty());

	DirtyReplacement->MarkDirty();
	Durin::Editor::Level::FLevelDocumentRevisionState::Activate(&Manager, DirtyReplacement);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*CleanReplacement).has_value());
	ASSERT_TRUE(Manager.GetPackageRevisionState(*DirtyReplacement).has_value());
	EXPECT_FALSE(Manager.GetPackageRevisionState(*DirtyReplacement)->bCheckpointValid);
	EXPECT_TRUE(DirtyReplacement->IsDirty());

	Durin::Editor::Level::FLevelDocumentRevisionState::Discard(&Manager, *DirtyReplacement);
	EXPECT_FALSE(Manager.GetPackageRevisionState(*DirtyReplacement).has_value());
	EXPECT_FALSE(DirtyReplacement->IsDirty());
}

TEST(FViewportCameraTransformTests, ClampsPitchAndBuildsOrthonormalDirections)
{
	Durin::Editor::Level::FViewportCameraTransform Camera;
	Camera.Rotate(0.0f, 200.0f);
	EXPECT_DOUBLE_EQ(Camera.GetPitch(), 89.0);
	EXPECT_NEAR(Durin::Math::Length(Camera.GetForwardVector()), 1.0, 1.e-8);
	EXPECT_NEAR(Durin::Math::Dot(Camera.GetForwardVector(), Camera.GetRightVector()), 0.0, 1.e-8);
	EXPECT_NEAR(Durin::Math::Dot(Camera.GetForwardVector(), Camera.GetUpVector()), 0.0, 1.e-8);
}

TEST(FViewportCameraTransformTests, MovesPansAndPreservesOrbitDistance)
{
	Durin::Editor::Level::FViewportCameraTransform Camera;
	const Durin::FVector3 InitialLocation = Camera.GetLocation();
	const Durin::FVector3 InitialPivot = Camera.GetOrbitPivot();
	Camera.MoveLocal({2.0, 0.0, 0.0});
	ExpectVectorNear(Camera.GetOrbitPivot() - InitialPivot, Camera.GetLocation() - InitialLocation);

	Camera.Pan(1.0f, -0.5f);
	const double Distance = Camera.GetOrbitDistance();
	Camera.Orbit(35.0f, 15.0f);
	EXPECT_NEAR(Durin::Math::Length(Camera.GetOrbitPivot() - Camera.GetLocation()), Distance, 1.e-8);
}

TEST(FViewportCameraTransformTests, FocusAndDollyRemainFiniteAtDegenerateDistance)
{
	Durin::Editor::Level::FViewportCameraTransform Camera;
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
	Durin::Editor::Level::FViewportCameraTransform Camera;
	Durin::Editor::Level::FLevelViewportCameraState State;
	State.Location = {10.0, 20.0, 30.0};
	State.OrbitPivot = {1.0, 2.0, 3.0};
	State.OrbitDistance = -4.0;
	State.Pitch = 200.0;
	State.Yaw = 123.0;
	Camera.SetState(State);
	const Durin::Editor::Level::FLevelViewportCameraState Actual = Camera.GetState();
	ExpectVectorNear(Actual.Location, State.Location);
	ExpectVectorNear(Actual.OrbitPivot, State.OrbitPivot);
	EXPECT_DOUBLE_EQ(Actual.OrbitDistance, 0.05);
	EXPECT_DOUBLE_EQ(Actual.Pitch, 89.0);
	EXPECT_DOUBLE_EQ(Actual.Yaw, State.Yaw);
}

TEST(FLevelViewportSessionSettingsTests, RoundTripsProjectsAndLevelsAndSkipsInvalidEntries)
{
	Durin::Editor::Level::FLevelViewportStateMap States;
	States["G:/Projects/A/A.dproject"]["/A/Levels/Main"] = {
		{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}, 7.0, -8.0, 9.0, 0.25f, 250000.0f, 120000.0f, 180000.0f};
	States["G:/Projects/B/B.dproject"]["/B/Levels/Other"] = {{10.0, 20.0, 30.0}, {40.0, 50.0, 60.0}, 70.0, -80.0, 90.0};
	Durin::FYamlDocument Document;
	Durin::FYamlNodeRef Root = Document.GetMutableRoot();
	Root.EnsureMap();
	Durin::Editor::Level::SaveLevelViewportStates(Root, States);
	Durin::FYamlNodeRef Invalid = Root.GetRef("LevelViewportStates").AppendMap();
	Invalid.SetChildValue("Project", "G:/Projects/A/A.dproject");
	Invalid.SetChildValue("Level", "/A/Levels/Broken");
	Invalid.AddSequence("Location").AppendValue(1.0).AppendValue(2.0);
	Durin::FYamlNodeRef Legacy = Root.GetRef("LevelViewportStates").AppendMap();
	Legacy.SetChildValue("Project", "G:/Projects/A/A.dproject");
	Legacy.SetChildValue("Level", "/A/Levels/Legacy");
	Legacy.AddSequence("Location").AppendValue(1.0).AppendValue(2.0).AppendValue(3.0);
	Legacy.AddSequence("OrbitPivot").AppendValue(4.0).AppendValue(5.0).AppendValue(6.0);
	Legacy.SetChildValue("OrbitDistance", 7.0);
	Legacy.SetChildValue("Pitch", -8.0);
	Legacy.SetChildValue("Yaw", 9.0);

	Durin::Editor::Level::FLevelViewportStateMap Loaded;
	Durin::Editor::Level::LoadLevelViewportStates(Document.GetRootView(), Loaded);
	ASSERT_EQ(Loaded.size(), 2u);
	ASSERT_EQ(Loaded.at("G:/Projects/A/A.dproject").size(), 2u);
	const Durin::Editor::Level::FLevelViewportCameraState& Main = Loaded.at("G:/Projects/A/A.dproject").at("/A/Levels/Main");
	ExpectVectorNear(Main.Location, {1.0, 2.0, 3.0});
	ExpectVectorNear(Main.OrbitPivot, {4.0, 5.0, 6.0});
	EXPECT_DOUBLE_EQ(Main.OrbitDistance, 7.0);
	EXPECT_DOUBLE_EQ(Main.Pitch, -8.0);
	EXPECT_DOUBLE_EQ(Main.Yaw, 9.0);
	EXPECT_FLOAT_EQ(Main.NearClip, 0.25f);
	EXPECT_FLOAT_EQ(Main.FarClip, 250000.0f);
	EXPECT_FLOAT_EQ(Main.ViewFadeStart, 120000.0f);
	EXPECT_FLOAT_EQ(Main.ViewRenderDistance, 180000.0f);
	const Durin::Editor::Level::FLevelViewportCameraState& LegacyState =
		Loaded.at("G:/Projects/A/A.dproject").at("/A/Levels/Legacy");
	const Durin::Editor::Level::FLevelViewportCameraState Defaults;
	EXPECT_FLOAT_EQ(LegacyState.NearClip, Defaults.NearClip);
	EXPECT_FLOAT_EQ(LegacyState.FarClip, Defaults.FarClip);
	EXPECT_FLOAT_EQ(LegacyState.ViewFadeStart, Defaults.ViewFadeStart);
	EXPECT_FLOAT_EQ(LegacyState.ViewRenderDistance, Defaults.ViewRenderDistance);
}
