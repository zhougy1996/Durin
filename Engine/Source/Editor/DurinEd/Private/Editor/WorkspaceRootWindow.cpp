#include "Editor/WorkspaceRootWindow.h"

#include "Asset/AssetOperations.h"
#include "Asset.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"

#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"

namespace Durin::Editor
{
	auto FWorkspaceRootWindow::Begin(const FWorkspaceRootWindowConfig& Config) -> FWorkspaceRootWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		WorkspaceUI::SetNextRootWindowClass();
		if (Config.bDockInEditorHost)
			ImGui::SetNextWindowDockID(WorkspaceUI::MakeHostDockSpaceId(WorkspaceUI::HostLayoutVersion), ImGuiCond_FirstUseEver);
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}

		const std::string DisplayName = Config.bDirty ? std::format("{} *", Config.DisplayName) : std::string(Config.DisplayName);
		const std::string RootWindowName = WorkspaceUI::MakeRootWindowName(DisplayName, Config.RootKey);
		if (Config.bZeroPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool bOpen = true;
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoCollapse | Config.AdditionalFlags |
			(Config.bDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
		FWorkspaceRootWindowState State;
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
			ImGui::DockBuilderGetNode(WorkspaceUI::MakeDockSpaceId(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion
			)) != nullptr)
		{
			WorkspaceUI::SubmitDockSpace(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion,
				ImVec2(0.0f, 0.0f),
				ImGuiDockNodeFlags_KeepAliveOnly
			);
		}
		return State;
	}

	auto FWorkspaceRootWindow::End() -> void
	{
		checkf(bWindowBegun, "An editor workspace root window must begin before it ends");
		ImGui::End();
		bWindowBegun = false;
	}

	auto FWorkspaceDocumentHost::RequestFocus(FDocumentId DocumentId) -> void
	{
		DocumentWindows[DocumentId.Value].RequestFocus();
	}

	auto FWorkspaceDocumentHost::DrawDocuments(
		FWorkspaceManager& WorkspaceManager,
		const FWorkspaceTypeId& WorkspaceType,
		std::string_view WorkspaceRootKey,
		const std::function<bool(const FDocumentTab&)>& CanDrawDocument,
		const std::function<void(const FDocumentTab&)>& DrawDocument,
		const std::function<void(const FDocumentTab&, bool)>& UpdateDocumentVisibility
	) -> bool
	{
		bool bWorkspaceActivated = false;
		std::vector<FDocumentId> CloseRequests;
		std::unordered_set<uint64> OpenDocumentIds;
		for (const FDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != WorkspaceType) continue;
			OpenDocumentIds.insert(Document.Id.Value);
			if (!CanDrawDocument(Document)) continue;

			FWorkspaceRootWindow& RootWindow = DocumentWindows[Document.Id.Value];
			const FWorkspaceRootWindowState WindowState = RootWindow.Begin({
				.DisplayName = Document.Label,
				.RootKey = WorkspaceUI::MakeDocumentRootKey(WorkspaceRootKey, Document.DocumentKey),
				.bDirty = Document.bDirty,
			});
			if (WindowState.bFocused || WindowState.bActivated)
			{
				bWorkspaceActivated = true;
				const FDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
				if (!ActiveDocument || ActiveDocument->Id != Document.Id)
					WorkspaceManager.ActivateDocument(Document.Id);
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
			WorkspaceManager.RequestCloseDocument(DocumentId);
		return bWorkspaceActivated;
	}

	auto FEditableAssetDocumentModel::Activate(
		const FDocumentTab& Document, DObject* Object) -> bool
	{
		DocumentHost.RequestFocus(Document.Id);
		if (!Object) return false;
		if (GEditor)
		{
			if (DPackage* Package = Object->GetPackage())
			{
				FTransactionManager& Transactions = GEditor->GetTransactionManager();
				if (!Transactions.GetPackageRevisionState(*Package))
				{
					if (Package->IsDirty()) Transactions.InvalidateSavedState(*Package);
					else Transactions.EstablishSavedState(*Package);
				}
			}
		}
		ActiveResourceId = Document.ResourceId;
		return true;
	}

	auto FEditableAssetDocumentModel::Close(std::string_view ResourceId) -> void
	{
		if (ActiveResourceId == ResourceId) ActiveResourceId.clear();
	}

	auto FEditableAssetDocumentModel::IsDirty(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage() && Object->GetPackage()->IsDirty();
	}

	auto FEditableAssetDocumentModel::CanSave(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage();
	}

	auto FEditableAssetDocumentModel::Save(DObject* Object,
		const std::function<bool()>& BeforeSave,
		const std::function<void(std::string)>& ReportError) -> bool
	{
		if (!CanSave(Object) || (BeforeSave && !BeforeSave())) return false;
		const Asset::FAssetResult Result = Asset::SavePackage(Object->GetPackage());
		if (Result)
		{
			if (GEditor) GEditor->GetTransactionManager().MarkSaved(*Object->GetPackage());
			return true;
		}
		if (ReportError) ReportError(Result.Message);
		return false;
	}

	auto FEditableAssetDocumentModel::Discard(DObject* Object,
		const std::function<void()>& BeforeDiscard) -> bool
	{
		if (!CanSave(Object)) return false;
		if (BeforeDiscard) BeforeDiscard();
		if (GEditor) GEditor->GetTransactionManager().ForgetPackage(*Object->GetPackage());
		Object->GetPackage()->ClearDirty();
		return true;
	}

	auto FEditableAssetDocumentModel::CanUndo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanUndo();
	}

	auto FEditableAssetDocumentModel::CanRedo() const -> bool
	{
		return GEditor && GEditor->GetTransactionManager().CanRedo();
	}

	auto FEditableAssetDocumentModel::GetUndoDescription() const -> std::string_view
	{
		return CanUndo()
			? GEditor->GetTransactionManager().GetUndoDescription() : std::string_view{};
	}

	auto FEditableAssetDocumentModel::GetRedoDescription() const -> std::string_view
	{
		return CanRedo()
			? GEditor->GetTransactionManager().GetRedoDescription() : std::string_view{};
	}

	auto FEditableAssetDocumentModel::Undo() -> bool
	{
		return CanUndo() && GEditor->GetTransactionManager().Undo();
	}

	auto FEditableAssetDocumentModel::Redo() -> bool
	{
		return CanRedo() && GEditor->GetTransactionManager().Redo();
	}

	auto FReadOnlyAssetDocumentModel::Activate(
		const FDocumentTab& Document, const DObject* Object) -> bool
	{
		DocumentHost.RequestFocus(Document.Id);
		if (!Object) return false;
		ActiveResourceId = Document.ResourceId;
		return true;
	}

	auto FReadOnlyAssetDocumentModel::Close(std::string_view ResourceId) -> void
	{
		if (ActiveResourceId == ResourceId) ActiveResourceId.clear();
	}
}
