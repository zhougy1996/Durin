#include <gtest/gtest.h>

#include "Workspace/WorkspaceUI.h"
#include "AssetCompatibilityWindow.h"
#include "Widgets/EditorNotificationOverlay.h"
#include "Notifications/Notification.h"
#include "Editor/AssetCompatibilityAuditTestSupport.h"
#include "MonaImGui.h"
#include "Panels/ConsolePanelLayout.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsWindow.h"
#include "Settings/HostSettings.h"
#include "Widgets/MWindow.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin::Editor::MainFrame
{
	struct FEditorSettingsWindowTestAccess
	{
		static auto SearchFor(FEditorSettingsWindow& Window, const char* Text) -> void
		{
			Window.Search.Clear();
			std::string_view(Text).copy(Window.Search.InputBuf, std::size(Window.Search.InputBuf) - 1);
			Window.Search.Build();
		}
	};

	TEST(FEditorSettingsTests, RoutesSearchAndKeepsBothPanesInsideMinimumSizeAtDifferentScales)
	{
		for (const float Scale : {1.0f, 2.0f})
		{
			SCOPED_TRACE(Scale);
			ImGuiContext* Context = ImGui::CreateContext();
			const float PreviousScale = MonaImGui::GetGlobalUIScale();
			struct FCleanup
			{
				ImGuiContext* Context;
				float PreviousScale;
				~FCleanup() { MonaImGui::SetGlobalUIScale(PreviousScale); ImGui::DestroyContext(Context); }
			} Cleanup{Context, PreviousScale};
			MonaImGui::SetGlobalUIScale(Scale);
			auto& IO = ImGui::GetIO();
			IO.IniFilename = nullptr;
			IO.DisplaySize = ImVec2(2200.0f, 1600.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			IO.Fonts->Build();
			int Draws = 0;
			FEditorSettingsPageRegistration Page({
				.Id = "test.settings.routing", .Label = "Routing",
				.Keywords = "unique-routing-keyword",
				.Draw = [&](std::string&) { ++Draws; ImGui::TextUnformatted("Test page content"); },
			});
			FEditorSettingsWindow Window;
			FHostSettings HostSettings;
			MWindow HostWindow;
			FEditorSettingsRegistry::Get().RequestOpen("test.settings.routing");
			const auto Frame = [&] {
				ImGui::NewFrame();
				Window.Draw(HostSettings, HostWindow);
				ImGui::Render();
			};
			Frame();
			ImGui::SetWindowSize("Settings###Durin.EditorHost.Settings", ImVec2(680.0f * Scale, 420.0f * Scale));
			Frame();
			ImGuiWindow* Settings = ImGui::FindWindowByName("Settings###Durin.EditorHost.Settings");
			ASSERT_NE(Settings, nullptr);
			ASSERT_EQ(Settings->DC.ChildWindows.Size, 2);
			const auto* Categories = Settings->DC.ChildWindows[0];
			const auto* Content = Settings->DC.ChildWindows[1];
			EXPECT_GT(Content->Size.x, 300.0f * Scale);
			EXPECT_LE(Categories->Pos.x + Categories->Size.x, Content->Pos.x);
			EXPECT_LE(Content->Pos.x + Content->Size.x, Settings->InnerRect.Max.x + 1.0f);
			EXPECT_LT(Content->Pos.y + Content->Size.y, Settings->InnerRect.Max.y);
			EXPECT_GT(Draws, 0);
			FEditorSettingsWindowTestAccess::SearchFor(Window, "unique-routing-keyword");
			Draws = 0;
			Frame();
			EXPECT_EQ(Draws, 1);
			FEditorSettingsWindowTestAccess::SearchFor(Window, "no-matching-setting");
			Draws = 0;
			Frame();
			EXPECT_EQ(Draws, 0);
			FEditorSettingsWindowTestAccess::SearchFor(Window, "unique-routing-keyword,-Routing");
			Frame();
			EXPECT_EQ(Draws, 0);
		}
	}

	TEST(FEditorSettingsTests, ScopedRegistrationRejectsDuplicatesAndTransfersOwnership)
	{
		auto& Registry = FEditorSettingsRegistry::Get();
		const auto HasPage = [&](std::string_view Id) {
			return std::ranges::any_of(Registry.GetPages(), [&](const auto& Page) { return Page.Id == Id; });
		};
		{
			FEditorSettingsPageRegistration First({.Id = "test.settings.first", .Label = "First", .Draw = [](std::string&) {}});
			ASSERT_TRUE(First.IsValid());
			FEditorSettingsPageRegistration Duplicate({.Id = "test.settings.first", .Label = "Duplicate", .Draw = [](std::string&) {}});
			EXPECT_FALSE(Duplicate.IsValid());
			FEditorSettingsPageRegistration Moved(std::move(First));
			EXPECT_FALSE(First.IsValid());
			EXPECT_TRUE(HasPage("test.settings.first"));
			FEditorSettingsPageRegistration Second({.Id = "test.settings.second", .Label = "Second", .Draw = [](std::string&) {}});
			Second = std::move(Moved);
			EXPECT_FALSE(HasPage("test.settings.second"));
			EXPECT_TRUE(HasPage("test.settings.first"));
		}
		EXPECT_FALSE(HasPage("test.settings.first"));
	}

	TEST(FEditorSettingsTests, OpenRequestsAreConsumedOnceAndKeepProjectPageIdentity)
	{
		auto& Registry = FEditorSettingsRegistry::Get();
		Registry.RequestOpen("project.general");
		EXPECT_EQ(Registry.TakeOpenRequest(), std::optional<std::string>("project.general"));
		EXPECT_FALSE(Registry.TakeOpenRequest().has_value());
		Registry.RequestOpen();
		const auto Request = Registry.TakeOpenRequest();
		ASSERT_TRUE(Request.has_value());
		EXPECT_TRUE(Request->empty());
	}

	TEST(FEditorHostToolTests, ClipsVariableHeightConsoleRecordsWithOverscan)
	{
		const std::array<float, 5> Offsets{0.0f, 20.0f, 60.0f, 80.0f, 140.0f};
		const FConsoleVisibleRange Middle =
			ResolveConsoleVisibleRange(Offsets, 55.0f, 30.0f);
		EXPECT_EQ(Middle.Begin, 0u);
		EXPECT_EQ(Middle.End, 4u);

		const FConsoleVisibleRange Bottom =
			ResolveConsoleVisibleRange(Offsets, 120.0f, 30.0f);
		EXPECT_EQ(Bottom.Begin, 2u);
		EXPECT_EQ(Bottom.End, 4u);
		EXPECT_EQ(ResolveConsoleVisibleRange({}, 0.0f, 100.0f).End, 0u);
	}

	TEST(FEditorHostToolTests, PlacesStatusBarDirectlyBelowHostDockSpace)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		IO.DisplaySize = ImVec2(1280.0f, 720.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();

		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(IO.DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Host", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		ImGui::PopStyleVar();

		const ImVec2 DockMin = ImGui::GetCursorScreenPos();
		const ImVec2 DockSize(1280.0f, 690.0f);
		const ImVec2 HostItemSpacing(ImGui::GetStyle().ItemSpacing.x, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, HostItemSpacing);
		WorkspaceUI::SubmitHostDockSpace(
			WorkspaceUI::HostLayoutVersion, DockSize);
		ImGui::PopStyleVar();

		ASSERT_TRUE(ImGui::BeginChild("StatusBar", ImVec2(0.0f, 30.0f)));
		EXPECT_FLOAT_EQ(ImGui::GetWindowPos().y, DockMin.y + DockSize.y);
		EXPECT_FLOAT_EQ(ImGui::GetWindowSize().y, 30.0f);
		ImGui::EndChild();
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::MainFrame

namespace Durin::Editor::MainFrame
{
	class FCanonicalResaveFeedbackTests : public FAssetCompatibilityAuditFixture {};

	struct FAssetCompatibilityWindowTestAccess
	{
		static auto Begin(FAssetCompatibilityWindow& Window, bool bHasPackage) -> void
		{
			FAssetCanonicalResavePlan Plan;
			Plan.RegistryRevision = GetAssetCatalogRevision();
			if (bHasPackage)
			{
				FAssetCanonicalResavePackagePlan Package;
				EXPECT_TRUE(FPackagePath::TryCreate("/AuditTests/Missing", Package.PackagePath));
				Package.Status = EAssetCanonicalResavePackageStatus::Ready;
				Plan.Packages.push_back(std::move(Package));
			}
			Window.PreviewMaintenancePlan = std::move(Plan);
			Window.BeginCanonicalResave();
		}
		static auto Cancel(FAssetCompatibilityWindow& Window) -> void
		{
			Window.bCancelMaintenance = true;
		}
		static auto Tick(FAssetCompatibilityWindow& Window) -> void { Window.TickCanonicalResave(); }
		static auto CancellationRequested(const FAssetCompatibilityWindow& Window) -> bool { return Window.bCancelMaintenance; }
		static auto IsPending(const FAssetCompatibilityWindow& Window) -> bool { return Window.PendingMaintenancePlan.has_value(); }
		static auto Message(const FAssetCompatibilityWindow& Window) -> const std::string& { return Window.MaintenanceMessage; }
	};

	TEST_F(FCanonicalResaveFeedbackTests, CancelStopsBeforeTheNextPackageIsApplied)
	{
		FAssetCompatibilityWindow Window;
		FAssetCompatibilityWindowTestAccess::Begin(Window, true);
		ASSERT_TRUE(FAssetCompatibilityWindowTestAccess::IsPending(Window));
		FAssetCompatibilityWindowTestAccess::Cancel(Window);
		FAssetCompatibilityWindowTestAccess::Tick(Window);
		EXPECT_FALSE(FAssetCompatibilityWindowTestAccess::IsPending(Window));
		EXPECT_NE(FAssetCompatibilityWindowTestAccess::Message(Window).find("cancelled after 0"), std::string::npos);
	}

	TEST_F(FCanonicalResaveFeedbackTests, FailedRevalidationAndEmptyCompletionReportBusinessOutcomes)
	{
		FAssetCompatibilityWindow Window;
		FAssetCompatibilityWindowTestAccess::Begin(Window, true);
		FAssetCompatibilityWindowTestAccess::Tick(Window);
		EXPECT_FALSE(FAssetCompatibilityWindowTestAccess::IsPending(Window));
		EXPECT_NE(FAssetCompatibilityWindowTestAccess::Message(Window).find("stopped after 0"), std::string::npos);
		FAssetCompatibilityWindowTestAccess::Begin(Window, false);
		FAssetCompatibilityWindowTestAccess::Tick(Window);
		EXPECT_NE(FAssetCompatibilityWindowTestAccess::Message(Window).find("completed: 0"), std::string::npos);
		EXPECT_FALSE(FAssetCompatibilityWindowTestAccess::IsPending(Window));
	}

	TEST_F(FCanonicalResaveFeedbackTests, ProjectChangeClearsPendingBatchAndCancellation)
	{
		FAssetCompatibilityWindow Window;
		FAssetCompatibilityWindowTestAccess::Begin(Window, true);
		FAssetCompatibilityWindowTestAccess::Cancel(Window);
		Window.ProjectChanged();
		EXPECT_FALSE(FAssetCompatibilityWindowTestAccess::IsPending(Window));
		EXPECT_FALSE(FAssetCompatibilityWindowTestAccess::CancellationRequested(Window));
	}
}

namespace Durin::Editor::MainFrame
{
	TEST(FNotificationPresentationTests, UnknownProgressHasNoFabricatedPercentage)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1000, 700);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();
		FNotificationManager Manager;
		const auto Id = Manager.BeginProgress({.Message = "Loading"});
		Manager.Tick(0);
		FEditorNotificationOverlay Overlay;
		auto Frame = [&]() {
			ImGui::NewFrame();
			Context->LogBuffer.clear();
			ImGui::LogToBuffer();
			Overlay.DrawToasts(Manager);
			const std::string Text = Context->LogBuffer.c_str();
			ImGui::LogFinish();
			ImGui::Render();
			return Text;
		};
		Frame();
		EXPECT_EQ(Frame().find('%'), std::string::npos);
		Manager.UpdateProgress(Id, 0.42f);
		Manager.Tick(0);
		EXPECT_NE(Frame().find("42%"), std::string::npos);
	}

	TEST(FNotificationPresentationTests, StatusBarExposesProgressCancellationWithoutAnAction)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1200, 700);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();
		FNotificationManager Manager;
		int Cancellations = 0;
		Manager.BeginProgress({.Message = "Importing textures", .Cancel = [&] { ++Cancellations; },
			.Presentation = ENotificationPresentation::StatusBar});
		Manager.Tick(0);
		FEditorNotificationOverlay Overlay;
		ImVec2 CancelPosition;
		auto Frame = [&]() {
			ImGui::NewFrame();
			ImGui::SetNextWindowSize(ImVec2(1100, 100));
			ImGui::Begin("StatusHost");
			Overlay.DrawStatusBar(Manager, EEditorStatusBarAction::None, 0);
			bool bFound = false;
			for (int Index = 0; Index < Context->Tables.GetMapSize(); ++Index)
			{
				const auto* Table = Context->Tables.GetByIndex(Index);
				if (!Table || Table->ColumnsCount != 5) continue;
				bFound = true;
				EXPECT_GT(Table->Columns[3].WidthGiven, 0);
				CancelPosition = ImVec2((Table->Columns[3].WorkMinX + Table->Columns[3].WorkMaxX) * 0.5f,
					Table->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f);
			}
			EXPECT_TRUE(bFound);
			ImGui::End();
			ImGui::Render();
		};
		Frame(); Frame();
		IO.AddMousePosEvent(CancelPosition.x, CancelPosition.y);
		Frame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		Frame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		Frame();
		EXPECT_EQ(Cancellations, 1);
		ASSERT_TRUE(Manager.GetStatusNotification());
		EXPECT_TRUE(Manager.GetStatusNotification()->bCancelRequested);
		Frame();
		EXPECT_EQ(Cancellations, 1);
	}
}
