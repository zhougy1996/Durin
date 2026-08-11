#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "SkeletalMeshEditorAPI.h"
#include "Widgets/SkeletalAssetPreview.h"

namespace Durin
{
	class DObject;

	// Hosts bounded read-only documents for the three skeletal asset classes.
	class MSkeletalAssetInspector final : public Editor::IWorkspace
	{
	public:
		explicit MSkeletalAssetInspector(Editor::FWorkspaceManager& InWorkspaceManager);
		SKELETALMESHEDITOR_API ~MSkeletalAssetInspector() override;
		SKELETALMESHEDITOR_API auto GetWorkspaceType() const -> const Editor::FWorkspaceTypeId& override;
		SKELETALMESHEDITOR_API auto OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult override;
		SKELETALMESHEDITOR_API auto ActivateDocument(const Editor::FDocumentTab& Document) -> void override;
		SKELETALMESHEDITOR_API auto RequestCloseDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentCloseResult override;
		auto SaveDocument(const Editor::FDocumentTab&) -> bool override { return false; }
		auto DiscardDocument(const Editor::FDocumentTab&) -> bool override { return false; }
		auto IsDocumentDirty(const Editor::FDocumentTab&) const -> bool override { return false; }
		auto CanSaveActiveDocument() const -> bool override { return false; }
		auto SaveActiveDocument() -> bool override { return false; }
		SKELETALMESHEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		SKELETALMESHEDITOR_API auto ResetLayout() -> void override;

	private:
		struct FDocumentState
		{
			TObjectPtr<DObject> Asset;
			TObjectPtr<DObject> PreviewPeer;
			std::vector<std::string> PreviewPeerPaths;
			int32 SelectedPreviewPeer = 0;
			std::unique_ptr<FSkeletalAssetPreview> Preview;
		};

		auto FindState(std::string_view DocumentKey) -> FDocumentState*;
		auto DrawDocument(const Editor::FDocumentTab& Document, FDocumentState& State) -> void;

		Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		Editor::FWorkspaceDocumentHost DocumentHost;
		std::string ErrorMessage;
		uint64 NextPreviewId = 1;
	};
}
