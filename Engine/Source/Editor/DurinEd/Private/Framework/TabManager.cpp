#include "Framework/TabManager.h"
#include "Framework/TabLayout.h"
#include "Documents/EditorDocumentManager.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin::Editor
{
	auto FEditorTabWindow::Begin(const FEditorTabWindowConfig& Config) -> FEditorTabWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		TabLayout::SetNextRootWindowClass();
		if (Config.bDockInEditorHost)
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
