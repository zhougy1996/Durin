#include "Editor/EditorWorkspaceRootWindow.h"

#include "Editor/EditorWorkspaceUI.h"

namespace Durin
{
	auto FEditorWorkspaceRootWindow::Begin(const FEditorWorkspaceRootWindowConfig& Config) -> FEditorWorkspaceRootWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		EditorWorkspaceUI::SetNextEditorRootWindowClass();
		if (Config.bDockInEditorHost)
			ImGui::SetNextWindowDockID(EditorWorkspaceUI::MakeEditorHostDockSpaceId(EditorWorkspaceUI::HostLayoutVersion), ImGuiCond_FirstUseEver);
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}

		const std::string DisplayName = Config.bDirty ? std::format("{} *", Config.DisplayName) : std::string(Config.DisplayName);
		const std::string RootWindowName = EditorWorkspaceUI::MakeEditorRootWindowName(DisplayName, Config.RootKey);
		if (Config.bZeroPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool bOpen = true;
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoCollapse | Config.AdditionalFlags |
			(Config.bDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
		FEditorWorkspaceRootWindowState State;
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
			ImGui::DockBuilderGetNode(EditorWorkspaceUI::MakeDockSpaceId(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion
			)) != nullptr)
		{
			EditorWorkspaceUI::SubmitDockSpace(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion,
				ImVec2(0.0f, 0.0f),
				ImGuiDockNodeFlags_KeepAliveOnly
			);
		}
		return State;
	}

	auto FEditorWorkspaceRootWindow::End() -> void
	{
		checkf(bWindowBegun, "An editor workspace root window must begin before it ends");
		ImGui::End();
		bWindowBegun = false;
	}

	auto FEditorWorkspaceDocumentHost::RequestFocus(FEditorDocumentId DocumentId) -> void
	{
		DocumentWindows[DocumentId.Value].RequestFocus();
	}

	auto FEditorWorkspaceDocumentHost::DrawDocuments(
		FEditorWorkspaceManager& WorkspaceManager,
		const FEditorWorkspaceTypeId& WorkspaceType,
		std::string_view WorkspaceRootKey,
		const std::function<bool(const FEditorDocumentTab&)>& CanDrawDocument,
		const std::function<void(const FEditorDocumentTab&)>& DrawDocument,
		const std::function<void(const FEditorDocumentTab&)>& PrepareDocument
	) -> bool
	{
		bool bWorkspaceActivated = false;
		std::vector<FEditorDocumentId> CloseRequests;
		std::unordered_set<uint64> OpenDocumentIds;
		for (const FEditorDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != WorkspaceType) continue;
			OpenDocumentIds.insert(Document.Id.Value);
			if (PrepareDocument) PrepareDocument(Document);
			if (!CanDrawDocument(Document)) continue;

			FEditorWorkspaceRootWindow& RootWindow = DocumentWindows[Document.Id.Value];
			const FEditorWorkspaceRootWindowState WindowState = RootWindow.Begin({
				.DisplayName = Document.Label,
				.RootKey = EditorWorkspaceUI::MakeEditorDocumentRootKey(WorkspaceRootKey, Document.DocumentKey),
				.bDirty = Document.bDirty,
			});
			if (WindowState.bFocused || WindowState.bActivated)
			{
				bWorkspaceActivated = true;
				const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
				if (!ActiveDocument || ActiveDocument->Id != Document.Id)
					WorkspaceManager.ActivateDocument(Document.Id);
			}
			if (WindowState.bVisible) DrawDocument(Document);
			RootWindow.End();
			if (WindowState.bCloseRequested) CloseRequests.push_back(Document.Id);
		}

		std::erase_if(DocumentWindows, [&](const auto& Entry) {
			return !OpenDocumentIds.contains(Entry.first);
		});
		for (FEditorDocumentId DocumentId : CloseRequests)
			WorkspaceManager.RequestCloseDocument(DocumentId);
		return bWorkspaceActivated;
	}
}
