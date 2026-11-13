#include "Framework/TabManager.h"
#include "Framework/TabLayout.h"
#include "Documents/EditorDocumentManager.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitHost.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin
{
	auto FEditorTabWindow::Begin(const FEditorTabWindowConfig& Config) -> FEditorTabWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		auto WindowClass = TabLayout::MakeRootWindowClass();
		if (Config.bNativeStandalone) WindowClass.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&WindowClass);
		if (Config.bNativeStandalone) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
		if (Config.bDockInEditorHost && !Config.bNativeStandalone)
			ImGui::SetNextWindowDockID(TabLayout::MakeMainFrameDockSpaceId(TabLayout::MainFrameLayoutVersion), ImGuiCond_FirstUseEver);
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}

		const std::string DisplayName = Config.bDirty ? std::format("{} *", Config.DisplayName) : std::string(Config.DisplayName);
		const std::string RootWindowName = TabLayout::MakeRootWindowName(DisplayName, Config.RootKey);
		if (Config.bZeroPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool bOpen = true;
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoCollapse | Config.AdditionalFlags |
			(Config.bNativeStandalone ? ImGuiWindowFlags_NoDocking : ImGuiWindowFlags_None) |
			(Config.bDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
		FEditorTabWindowState State;
		State.bVisible = ImGui::Begin(RootWindowName.c_str(), &bOpen, Flags);
		bWindowBegun = true;
		if (Config.bZeroPadding) ImGui::PopStyleVar();
		State.bFocused = State.bVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const ImGuiWindow* Window = ImGui::GetCurrentWindow();
		// Begin() may return true one frame before a queued dock-tab selection becomes visible.
		// Use the dock state directly so document activation follows the tab the user actually sees.
		const bool bDockTabSelected = Window->DockIsActive && Window->DockTabIsVisible;
		// A selected dock tab may not transfer focus to one of its child controls.
		State.bActivated = bDockTabSelected && !bWasDockTabSelected;
		bWasDockTabSelected = bDockTabSelected;
		State.bCloseRequested = !bOpen;

		if (!State.bVisible && Config.InternalDockSpace &&
			ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(
				Config.InternalDockSpace->ToolkitId,
				Config.InternalDockSpace->LayoutVersion
			)) != nullptr)
		{
			TabLayout::SubmitDockSpace(
				Config.InternalDockSpace->ToolkitId,
				Config.InternalDockSpace->LayoutVersion,
				ImVec2(0.0f, 0.0f),
				ImGuiDockNodeFlags_KeepAliveOnly
			);
		}
		return State;
	}

	auto FEditorTabWindow::End() -> void
	{
		checkf(bWindowBegun, "An editor workspace root window must begin before it ends");
		ImGui::End();
		bWindowBegun = false;
	}

	auto FTabManager::RequestFocus(FDocumentId DocumentId) -> void
	{
		DocumentWindows[DocumentId.Value].RequestFocus();
		bSessionFocusRequested = true;
		FocusDocument = DocumentId;
	}

	auto FTabManager::DrawDocuments(
		FEditorDocumentManager& DocumentManager,
		const FAssetEditorToolkitId& ToolkitId,
		std::string_view WorkspaceRootKey,
		const std::function<bool(const FEditorDocument&)>& CanDrawDocument,
		const std::function<void(const FEditorDocument&)>& DrawDocument,
		const std::function<void(const FEditorDocument&, bool)>& UpdateDocumentVisibility
	) -> bool
	{
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(DocumentManager.FindToolkit(ToolkitId));
		if (const auto Session = Toolkit; Session && Session->GetEditorInstance())
		{
			const auto Documents = DocumentManager.GetDocuments();
			if (Documents.empty()) return false;
			const auto Host = Session->GetToolkitHost();
			if (bSessionFocusRequested) { SessionWindow.RequestFocus(); bSessionFocusRequested = false; }
			const std::string RootKey = std::string(WorkspaceRootKey);
			const std::string Label = Documents.size() == 1 ? Documents.front().Label : std::format("{} ({} documents)", Session->GetEditorName().ToString(), Documents.size());
			FEditorTabWindowConfig Config{.DisplayName = Label, .RootKey = RootKey,
				.bDirty = std::ranges::any_of(Documents, [](const auto& Document) { return Document.bDirty; }),
				.AdditionalFlags = ImGuiWindowFlags_NoSavedSettings};
			if (Host) Host->PrepareRootWindow(Config);
			const auto WindowState = SessionWindow.Begin(Config);
			if (Host) Host->OnRootWindowShown(ImGui::GetWindowViewport()->ID);
			const bool bActivated = WindowState.bFocused || WindowState.bActivated;
			std::vector<FDocumentId> CloseRequests;
			if (WindowState.bVisible && Documents.size() > 1 && ImGui::BeginTabBar("Documents"))
			{
				for (const auto& Document : Documents)
				{
					bool bOpen = true;
					const auto* Active = DocumentManager.GetActiveDocument();
					const auto Flags = FocusDocument == Document.Id ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
					const auto TabName = std::format("{}###Document.{}", Document.Label, Document.Id.Value);
					if (ImGui::BeginTabItem(TabName.c_str(), &bOpen, Flags))
					{
						if (!Active || Active->Id != Document.Id) DocumentManager.ActivateDocument(Document.Id);
						ImGui::EndTabItem();
					}
					if (!bOpen) CloseRequests.push_back(Document.Id);
				}
				ImGui::EndTabBar();
				FocusDocument = {};
			}
			const auto* Active = DocumentManager.GetActiveDocument();
			const std::optional<FEditorDocument> ActiveSnapshot = Active ? std::optional(Toolkit->GetPresentationDocument(*Active)) : std::nullopt;
			for (const auto& Document : Documents)
				if (UpdateDocumentVisibility) UpdateDocumentVisibility(Toolkit->GetPresentationDocument(Document), WindowState.bVisible && ActiveSnapshot && ActiveSnapshot->Id == Document.Id);
			if (WindowState.bVisible && ActiveSnapshot && CanDrawDocument(*ActiveSnapshot))
			{
				DrawDocument(*ActiveSnapshot);
				if (auto Base = std::dynamic_pointer_cast<FBaseAssetToolkit>(Toolkit)) Base->DrawRegisteredUI();
			}
			SessionWindow.End();
			for (auto Id : CloseRequests) DocumentManager.RequestCloseDocument(Id);
			if (WindowState.bCloseRequested) Session->CloseWindow(EAssetEditorCloseReason::CloseAllAssetEditors);
			return bActivated;
		}
		bool bWorkspaceActivated = false;
		std::vector<FDocumentId> CloseRequests;
		std::unordered_set<uint64> OpenDocumentIds;
		for (const FEditorDocument& Document : DocumentManager.GetDocuments())
		{
			if (Document.ToolkitId != ToolkitId) continue;
			OpenDocumentIds.insert(Document.Id.Value);
			if (!CanDrawDocument(Document)) continue;

			FEditorTabWindow& RootWindow = DocumentWindows[Document.Id.Value];
			const FEditorTabWindowState WindowState = RootWindow.Begin({
				.DisplayName = Document.Label,
				.RootKey = TabLayout::MakeDocumentRootKey(WorkspaceRootKey, Document.DocumentKey),
				.bDirty = Document.bDirty,
			});
			if (WindowState.bFocused || WindowState.bActivated)
			{
				bWorkspaceActivated = true;
				const FEditorDocument* ActiveDocument = DocumentManager.GetActiveDocument();
				if (!ActiveDocument || ActiveDocument->Id != Document.Id)
					DocumentManager.ActivateDocument(Document.Id);
			}
			if (UpdateDocumentVisibility)
				UpdateDocumentVisibility(Document, WindowState.bVisible);
			if (WindowState.bVisible) DrawDocument(Document);
			RootWindow.End();
			if (WindowState.bCloseRequested) CloseRequests.push_back(Document.Id);
		}

		std::erase_if(DocumentWindows, [&](const auto& Entry) {
			return !OpenDocumentIds.contains(Entry.first);
		});
		for (FDocumentId DocumentId : CloseRequests)
			DocumentManager.RequestCloseDocument(DocumentId);
		return bWorkspaceActivated;
	}

}
