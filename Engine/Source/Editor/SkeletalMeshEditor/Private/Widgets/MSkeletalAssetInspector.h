#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"
#include "SkeletalMeshEditorAPI.h"
#include "Widgets/SkeletalAssetPreview.h"

namespace Durin
{
	class DObject;

	// Hosts bounded read-only documents for the three skeletal asset classes.
	class MSkeletalAssetInspector final : public IEditorWorkspace
	{
	public:
		explicit MSkeletalAssetInspector(FEditorWorkspaceManager& InWorkspaceManager);
		SKELETALMESHEDITOR_API ~MSkeletalAssetInspector() override;
		SKELETALMESHEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		SKELETALMESHEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult override;
		SKELETALMESHEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		SKELETALMESHEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult override;
		auto SaveDocument(const FEditorDocumentTab&) -> bool override { return false; }
		auto DiscardDocument(const FEditorDocumentTab&) -> bool override { return false; }
		auto IsDocumentDirty(const FEditorDocumentTab&) const -> bool override { return false; }
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
		auto DrawDocument(const FEditorDocumentTab& Document, FDocumentState& State) -> void;

		FEditorWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		FEditorWorkspaceDocumentHost DocumentHost;
		std::string ErrorMessage;
		uint64 NextPreviewId = 1;
	};
}
